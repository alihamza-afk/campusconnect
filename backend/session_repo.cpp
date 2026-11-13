// ---------------------------------------------------------------------------
// session_repo.cpp - the SQL for the `sessions` table.
//
// "Log out everywhere" is one DELETE by user_id. It works only because the
// sessions are server-side rows: if sessions were self-contained tokens (JWT),
// the server could not revoke them without a denylist, which is exactly this
// table with extra steps.
// ---------------------------------------------------------------------------
#include "session_repo.h"

#include <ctime>

namespace {
// A user is unlikely to need more than a few devices at once. Capping the rows
// keeps one account from growing the table without bound.
const int MAX_SESSIONS_PER_USER = 10;
}

SessionRepo::SessionRepo(Database& db) : db_(db) {}

void SessionRepo::create(int userId, const std::string& tokenHash, long long expiresAt) {
    Statement insert = db_.prepare(
        "INSERT INTO sessions (token_hash, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)");
    insert.bind(1, tokenHash);
    insert.bind(2, userId);
    insert.bind(3, static_cast<long long>(std::time(nullptr)));  // created now
    insert.bind(4, expiresAt);                                   // caller decides lifetime
    insert.run();

    // Keep the newest MAX_SESSIONS_PER_USER rows for this user. The subquery
    // picks the ones to keep; NOT IN deletes the rest.
    Statement trim = db_.prepare(
        "DELETE FROM sessions WHERE user_id = ? AND token_hash NOT IN ("
        "  SELECT token_hash FROM sessions WHERE user_id = ? ORDER BY created_at DESC LIMIT ?)");
    trim.bind(1, userId);
    trim.bind(2, userId);
    trim.bind(3, MAX_SESSIONS_PER_USER);
    trim.run();
}

std::optional<int> SessionRepo::findUserId(const std::string& tokenHash, long long now) {
    // The expiry check is part of the query, so an expired session can never be
    // accepted, even if the cleanup has not run yet.
    Statement s = db_.prepare("SELECT user_id FROM sessions WHERE token_hash = ? AND expires_at > ?");
    s.bind(1, tokenHash);
    s.bind(2, now);
    if (!s.step()) return std::nullopt;
    return s.columnInt(0);
}

bool SessionRepo::remove(const std::string& tokenHash) {
    Statement s = db_.prepare("DELETE FROM sessions WHERE token_hash = ?");
    s.bind(1, tokenHash);
    s.run();
    return db_.changes() > 0;
}

int SessionRepo::removeAllForUser(int userId) {
    Statement s = db_.prepare("DELETE FROM sessions WHERE user_id = ?");
    s.bind(1, userId);
    s.run();
    return db_.changes();
}

int SessionRepo::pruneExpired(long long now) {
    Statement s = db_.prepare("DELETE FROM sessions WHERE expires_at <= ?");
    s.bind(1, now);
    s.run();
    return db_.changes();
}

int SessionRepo::countForUser(int userId) {
    Statement s = db_.prepare("SELECT COUNT(*) FROM sessions WHERE user_id = ?");
    s.bind(1, userId);
    return s.step() ? s.columnInt(0) : 0;
}

long long SessionRepo::countActive(long long now) {
    Statement s = db_.prepare("SELECT COUNT(*) FROM sessions WHERE expires_at > ?");
    s.bind(1, now);
    return s.step() ? s.columnInt64(0) : 0;
}
