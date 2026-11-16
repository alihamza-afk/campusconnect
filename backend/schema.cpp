// ---------------------------------------------------------------------------
// schema.cpp - the database layout, and how it gets created / upgraded.
//
// The DDL is a raw string in this file instead of a separate .sql file so the
// binary works from any working directory (no "where is schema.sql?" problem).
// It is still plain SQL you can copy into the sqlite3 command line.
//
// Migrations: PRAGMA user_version in the database header stores the schema
// version. migrate() runs only the steps the file has not seen yet, so an old
// database is upgraded in place instead of being thrown away. Phase 2 adds
// migration 2 (sessions table), phase 6 migration 3 (claims table), etc.
// ---------------------------------------------------------------------------
#include "schema.h"

// Version 1: everything Part 1 needs - accounts and notices.
//
// Notes worth knowing for the viva:
//  * CHECK constraints are the last line of defence: even if a bug in the C++
//    validation code let a bad value through, the database would refuse it.
//  * The UNIQUE constraint on email is what makes "one account per email"
//    true under concurrency, not the SELECT that checks beforehand.
//  * Foreign keys + ON DELETE CASCADE: deleting a user removes their notices,
//    so there can never be a notice without an owner.
//  * INTEGER timestamps are Unix seconds (portable, easy to compare and sort).
static const char* SCHEMA_SQL = R"SQL(
CREATE TABLE IF NOT EXISTS users (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    email      TEXT    NOT NULL UNIQUE,
    name       TEXT    NOT NULL,
    pass_hash  TEXT    NOT NULL,
    role       TEXT    NOT NULL DEFAULT 'user' CHECK (role IN ('user', 'admin')),
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS items (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    owner_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    type        TEXT    NOT NULL CHECK (type IN ('sell', 'lend', 'lost', 'found')),
    title       TEXT    NOT NULL,
    description TEXT    NOT NULL DEFAULT '',
    category    TEXT    NOT NULL DEFAULT 'Other',
    location    TEXT    NOT NULL DEFAULT '',
    price       REAL    NOT NULL DEFAULT 0 CHECK (price >= 0 AND price <= 1000000),
    status      TEXT    NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'closed')),
    created_at  INTEGER NOT NULL
);

-- The two sorts the board offers. A B-tree index keeps them cheap even when
-- the table grows, instead of sorting every row on every request.
CREATE INDEX IF NOT EXISTS idx_items_status_created ON items(status, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_items_status_price   ON items(status, price ASC);
-- Used by "my notices" style queries and by the ON DELETE CASCADE check.
CREATE INDEX IF NOT EXISTS idx_items_owner          ON items(owner_id);
)SQL";

// Version 2: login sessions. The cookie holds a random token; this table holds
// only its SHA-256 hash, so a stolen database file cannot be used to log in.
static const char* SESSION_SQL = R"SQL(
CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT    PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_user    ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);
)SQL";

// Version 3: single-use email links for verification and password reset.
// Only the SHA-256 hash of the token is stored, never the token itself.
static const char* EMAIL_TOKEN_SQL = R"SQL(
CREATE TABLE IF NOT EXISTS email_tokens (
    token_hash  TEXT    PRIMARY KEY,
    user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    kind        TEXT    NOT NULL CHECK (kind IN ('verify', 'reset')),
    created_at  INTEGER NOT NULL,
    expires_at  INTEGER NOT NULL,
    consumed_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_email_tokens_user    ON email_tokens(user_id, kind);
CREATE INDEX IF NOT EXISTS idx_email_tokens_expires ON email_tokens(expires_at);
)SQL";

// Version 4: fixed-window counters for rate limiting, kept in SQLite so they
// survive a restart. A key looks like "login-email:someone@x" or "login-ip:1.2.3.4".
static const char* RATE_LIMIT_SQL = R"SQL(
CREATE TABLE IF NOT EXISTS rate_limits (
    key          TEXT    PRIMARY KEY,
    hits         INTEGER NOT NULL,
    window_start INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_rate_limits_window ON rate_limits(window_start);
)SQL";

// Version 5: claim verification for lost & found.
//   items.proof    - a private detail only the real owner would know
//   items.contact  - how the owner can be reached; shown only after approval
//   claims         - one row per person asking to claim a lost/found notice
static const char* CLAIM_SQL = R"SQL(
ALTER TABLE items ADD COLUMN proof TEXT;
ALTER TABLE items ADD COLUMN contact TEXT;

CREATE TABLE IF NOT EXISTS claims (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id     INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    claimant_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    answer      TEXT    NOT NULL,
    message     TEXT    NOT NULL DEFAULT '',
    contact     TEXT    NOT NULL,
    status      TEXT    NOT NULL DEFAULT 'pending' CHECK (status IN ('pending','approved','rejected')),
    created_at  INTEGER NOT NULL,
    decided_at  INTEGER,
    decided_by  INTEGER REFERENCES users(id),
    UNIQUE(item_id, claimant_id)
);
CREATE INDEX IF NOT EXISTS idx_claims_item_status ON claims(item_id, status);
CREATE INDEX IF NOT EXISTS idx_claims_claimant     ON claims(claimant_id);
)SQL";

void migrate(Database& db) {
    int version = db.userVersion();
    if (version >= SCHEMA_VERSION) return;

    if (version < 1) {
        Transaction tx(db);
        db.exec(SCHEMA_SQL);
        tx.commit();
        // Written after COMMIT: PRAGMA user_version changes the file header,
        // and keeping it out of the DDL transaction makes the intent clearer.
        db.setUserVersion(1);
        version = 1;
    }
    if (version < 2) {
        Transaction tx(db);
        db.exec(SESSION_SQL);
        tx.commit();
        db.setUserVersion(2);
    }
    if (version < 3) {
        Transaction tx(db);
        // New column: NULL means "not verified yet".
        db.exec("ALTER TABLE users ADD COLUMN email_verified_at INTEGER");
        db.exec(EMAIL_TOKEN_SQL);
        // Accounts that existed before this migration are treated as verified,
        // so an upgrade never locks existing users out of posting. New accounts
        // are created with NULL and have to verify.
        db.exec("UPDATE users SET email_verified_at = created_at WHERE email_verified_at IS NULL");
        tx.commit();
        db.setUserVersion(3);
    }
    if (version < 4) {
        Transaction tx(db);
        db.exec(RATE_LIMIT_SQL);
        tx.commit();
        db.setUserVersion(4);
    }
    if (version < 5) {
        Transaction tx(db);
        db.exec(CLAIM_SQL);
        tx.commit();
        db.setUserVersion(5);
    }
    if (version < 6) {
        Transaction tx(db);
        // Phase 8: user banning, item images, and in-app notifications.
        db.exec("ALTER TABLE users ADD COLUMN banned_at INTEGER");
        db.exec("ALTER TABLE items ADD COLUMN image_mime TEXT");
        db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS notifications (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    kind       TEXT    NOT NULL,
    item_id    INTEGER REFERENCES items(id) ON DELETE CASCADE,
    text       TEXT    NOT NULL,
    created_at INTEGER NOT NULL,
    read_at    INTEGER
);
CREATE INDEX IF NOT EXISTS idx_notifications_user ON notifications(user_id, read_at);
)SQL");
        tx.commit();
        db.setUserVersion(6);
    }

    if (version < 7) {
        Transaction tx(db);
        db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS purchase_requests (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    buyer_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    offered_price REAL NOT NULL CHECK (offered_price >= 0 AND offered_price <= 1000000),
    message TEXT NOT NULL DEFAULT '',
    buyer_contact TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending'
        CHECK (status IN ('pending', 'accepted', 'rejected')),
    created_at INTEGER NOT NULL,
    decided_at INTEGER,
    decided_by INTEGER REFERENCES users(id)
);

CREATE INDEX IF NOT EXISTS idx_purchase_requests_item_status
    ON purchase_requests(item_id, status);

CREATE INDEX IF NOT EXISTS idx_purchase_requests_buyer
    ON purchase_requests(buyer_id);

CREATE UNIQUE INDEX IF NOT EXISTS idx_purchase_requests_one_pending
    ON purchase_requests(item_id, buyer_id)
    WHERE status = 'pending';
)SQL");
        tx.commit();
        db.setUserVersion(7);
    }
}
