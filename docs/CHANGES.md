# CampusConnect change notes (Phases 0-7)

This revision moves CampusConnect's storage from two JSON files to a SQLite database,
and cleans up the repository. **The HTTP API and the frontend are unchanged** - the
browser behaves exactly as before.

---

## Why this was done

Part 1 rewrote `items.json` and `users.json` in full on every single change:

- no transactions: a crash halfway through a save could corrupt or truncate the file;
- no constraints: a bug in the C++ validation could save an impossible row;
- O(n) write per change, and all notices re-read/re-parsed on every start;
- no concurrent-writer story at all.

Every one of these is a job a relational database does for free. SQLite was chosen
because it is public domain, needs no server or account, works offline during a demo,
and compiles into the project with one `-lsqlite3` flag (already present on macOS).

The DSA structures from Part 1 are **not** removed. They become the in-memory read
index, rebuilt from the database at start-up.

---

## What changed, file by file

| File | Before | After | Why |
|---|---|---|---|
| `backend/db.h` | *did not exist* | New: RAII wrapper over the SQLite C API (`Database`, `Statement`, `Transaction`) | The C API needs manual `finalize()` and returns error codes; RAII makes leaks and ignored errors impossible |
| `backend/schema.h` | *did not exist* | New: table DDL in a raw string + versioned `migrate()` | Schema lives with the code; migrations upgrade old databases instead of deleting them |
| `backend/user_repo.h` | *did not exist* | New: all SQL for `users`, plus the `User` record | SQL in one place per table; prepared statements everywhere |
| `backend/item_repo.h` | *did not exist* | New: all SQL for `items` | Same, plus the JOIN that supplies the owner's display name |
| `backend/legacy_json.h` | *did not exist* | New: one-time import of the old JSON files | Existing accounts and notices are not lost |
| `backend/auth.h` | Kept `std::map<int, User>` of accounts in RAM | Accounts read/write through `UserRepo`; same hashing, lockout, sessions | One source of truth for accounts; no duplicate in-memory copy to drift |
| `backend/store.h` | Loaded items from a JSON file in its constructor; saved the whole file on every write | Loads once from `ItemRepo`; every write goes to the database first, then memory | Durability without giving up O(1)/O(log n) reads; the write order is what makes crashes safe |
| `backend/storage.h` | JSON (de)serialisation + atomic file writes | **Deleted**; its import job moved to `legacy_json.h` | No JSON file writes left to go wrong |
| `backend/main.cpp` | Opened `Store(path)` / `Auth(path)` | Opens the database, runs migrations, imports legacy rows, wires repos into Store/Auth; adds an exception handler and `CC_PORT` | Start-up order matters: migrate → import → build indexes. The exception handler keeps a bad query from killing the server |
| `backend/httplib.h`, `json.hpp` | Downloaded by hand per README | Downloaded by `make`, checksum-verified | A fresh clone builds with one command |
| `.env.example` | *did not exist* | Documents every setting | Discoverability |
| `Makefile` | *did not exist* | `make`, `make run`, `make test`, `make asan`, `make clean` | One command; no long g++ line to mistype |
| `scripts/fetch_deps.sh` | *did not exist* | Downloads the two headers, verifies SHA-256 | Same versions/checksums as the Dockerfile |
| `tests/test_units.cpp` | *did not exist* | 79 checks: DSA, database, repos, auth, legacy import, restart | Proof the migration works |
| `scripts/smoke.sh` | *did not exist* | 25 end-to-end checks over real HTTP, including a restart | Catches wiring bugs unit tests cannot |
| `Dockerfile` | `-lsodium` only | `libsqlite3-dev` + `-lsqlite3` | Container build matches native build |
| `README.md` | JSON-file setup | Database section, migrations, tests, structure, updated limitations | The README is what a panel reads first |
| `.gitignore` | Basic | Ignores `campus.db*`, binaries, dSYM, `.env` | A database with password hashes must never be committed |

### Removed junk

- `test` (a 94 KB arm64 binary) and `test.cpp` - an unrelated MongoDB connectivity probe.
- All `.DS_Store` files.
- `.claude/worktrees/...` - a leftover worktree copy of the repository.
- `backend/storage.h` - superseded.
- the stale `claude/understanding-clarification-672e51` branch can be deleted with
  `git branch -D claude/understanding-clarification-672e51` (it has no unique commits).

---

## What did NOT change

- **Every HTTP route and response shape.** `/api/register`, `/api/login`, `/api/me`,
  `/api/items` (GET/POST), `/api/items/:id/close`, `DELETE /api/items/:id`.
- **The frontend.** `frontend/` is byte-identical; it talks to the same API.
- **The security rules:** Argon2id, cookie flags, Origin check, CSP, lockout, ownership
  checks, role handling.
- **The DSA structures.** `hashtable.h`, `trie.h`, `heap.h` are untouched. Only their
  source of data changed.

---

## Visible behaviour differences

1. Data is stored in `backend/data/campus.db` instead of two JSON files.
2. On first start, existing `items.json` / `users.json` are imported automatically:
   ```
   Imported 1 account(s) from data/users.json
   Imported 1 notice(s) from data/items.json
   ```
3. `POST /api/items` still returns the created notice, as before.
4. The server logs the database path at start-up and returns a generic JSON error on an
   unexpected internal failure instead of an empty response.
5. `CC_PORT` can change the listening port (the smoke test uses 18080).

---

## How it was verified

```
make                     # builds with -Wall -Wextra, no warnings
make test                # 79 checks, 0 failed
sh scripts/smoke.sh      # 25 checks, 0 failed (real server, real restart)
make asan                # same code under AddressSanitizer + UBSan
```

The smoke test covers: legacy import, duplicate-email rejection, short-password
rejection, unauthenticated access, posting, listing, prefix search, price sorting,
closing/deleting your own notice, being blocked from someone else's (403), 404 on a
missing notice, CSP header, static frontend, session loss on restart, account survival
on restart, and deleted notices staying deleted.

---

## Migration / rollback

- **Forward:** just start the server. Migrations run automatically; old JSON data is
  imported once if the tables are empty.
- **Rollback:** stop the server, `git checkout` the previous commit, delete
  `backend/data/campus.db*`, and the old JSON files are used again (they were never
  modified by the import). Nothing is destroyed by an upgrade.
- **Fresh start:** stop the server, delete `backend/data`.
- **Backup:** `sqlite3 backend/data/campus.db "VACUUM INTO 'backup.db'"`.

---

## Not done yet (deliberately deferred to the next phases)

- Sessions still live in RAM (Phase 2 puts them in a `sessions` table).
- No email verification, per-IP rate limiting, HTTPS, or claim flow yet (Phases 3-6).
- The hash table still has a fixed 101 buckets; the heap still sorts the whole board per
  request (Phase 7).


---

## Phase 1.5 (this revision): header/source split

Before this revision every class was implemented inside its header (header-only).
That is fine for a small program, but it has two costs: every `.cpp` that includes
the header re-compiles the whole implementation, and if two translation units ever
included it, the linker would see duplicate definitions (the One Definition Rule).

Now each class is **declared** in its `.h` and **defined** in exactly one `.cpp`:

| Header | Declares | Implemented in | Notes |
|---|---|---|---|
| `db.h` | `DbError`, `Statement`, `Database`, `Transaction` | `db.cpp` | |
| `schema.h` | `SCHEMA_VERSION`, `migrate()` | `schema.cpp` | The SQL text is now an implementation detail |
| `user_repo.h` | `User`, `UserRepo` | `user_repo.cpp` | |
| `item_repo.h` | `ItemRepo` | `item_repo.cpp` | |
| `legacy_json.h` | `importLegacyJson()` | `legacy_json.cpp` | Header uses forward declarations only |
| `store.h` | `Store` | `store.cpp` | `heap.h` moved out of the header into the `.cpp` |
| `auth.h` | `Auth` | `auth.cpp` | `sodium.h` moved out; header no longer drags libsodium everywhere |
| `hashtable.h` | `HashTable` | `hashtable.cpp` | |
| `trie.h` | `Trie` (incl. nested `Node`) | `trie.cpp` | `MAX_WORDS` moved into the `.cpp` |
| `heap.h` | `Heap<T>` | **stays header-only** | A template must be visible where it is instantiated |
| `item.h` | `Item` struct | **stays header-only** | A shared data type has no implementation to move |

Other changes that follow from the split:

- `Makefile` compiles each `.cpp` to `build/*.o` and links them. Touching
  `store.cpp` rebuilds `store.o` and relinks - it does not recompile the world.
- `tests/test_units.cpp` is linked against the objects (all sources except `main.cpp`).
- `Dockerfile` builds `*.cpp` instead of `main.cpp`.
- `.gitignore` ignores `build/`.
- Headers got shorter and stopped leaking third-party includes: `auth.h` went from
  244 lines to 72, `store.h` from 126 to 48, and `legacy_json.h` from 123 to 20.

No behaviour changed. `make test` (79 checks) and `scripts/smoke.sh` (25 checks)
pass exactly as before, plus a sanitizer run over the split build.


---

## Phase 2 (this revision): sessions in SQLite

Until now a login session was an entry in a `std::unordered_map` inside the Auth object.
If the process restarted, the map was empty and every user was logged out - even though
their accounts and notices were safely on disk.

### What changed

| File | Change |
|---|---|
| `schema.h` / `schema.cpp` | `SCHEMA_VERSION` is now 2. Migration 2 creates the `sessions` table with indexes on `user_id` and `expires_at`. |
| `session_repo.h` / `session_repo.cpp` | **New.** SQL for sessions: create (with a per-user cap), look up by token hash + expiry, revoke one, revoke all, prune expired. |
| `auth.h` / `auth.cpp` | The in-memory session map is gone. `Auth` now takes a `SessionRepo&`. It hashes the token with SHA-256 before storing or looking it up, prunes expired rows at start-up and on login, and adds `logoutAll(userId)`. Only the failed-login counters stay in memory (Phase 4 moves them). |
| `main.cpp` | Creates the `SessionRepo`, passes it to `Auth`, and adds `POST /api/logout-all`. |
| `frontend/index.html`, `frontend/app.js` | New **Log out all** button that calls the endpoint and returns to the login screen. |
| `Makefile` | Builds `session_repo.cpp` too. |
| `tests/test_units.cpp` | New `testSessions()`: hashed tokens at rest, session survives a "restart" (new Auth on the same DB), logout one, logout all, expired row refused, per-user cap, and a real Phase 1 -> Phase 2 migration test. Now 103 checks. |
| `scripts/smoke.sh` | Restart now *keeps* the session; checks the stored hash length is 64, logout-all ends both devices, leaves other accounts alone, and login works afterwards. Now 32 checks. |

### Why it is built this way

- **Hash, do not store, the token.** The cookie holds 256 random bits; the database holds
  `SHA-256(token)`. A leaked `campus.db` cannot be replayed as a login. This is the same
  idea as password hashing, but faster, because the token already has high entropy.
- **Server-side sessions, not a JWT.** A self-contained token cannot be revoked without a
  denylist; a denylist is this table with extra steps. A row delete is a real logout.
- **Expiry in the query, not only in cleanup.** `WHERE expires_at > ?` means an expired
  session is refused even if the cleanup has not run yet.
- **Bounded growth.** Expired rows are deleted at start-up and on each login; each account
  keeps at most 10 sessions (the trim statement uses a subquery: keep the newest 10).
  `created_at`/`expires_at` are indexed, so those deletes stay cheap.

### Visible behaviour changes

1. **Restarting the server no longer logs you out** (the previous limitation is gone).
2. **Log out all** in the header ends the session on every device for that account.
3. A user with more than 10 active sessions loses the oldest one.
4. `POST /api/logout-all` returns `{"ok": true, "ended": N}`.

### Upgrade path

Existing databases upgrade in place on start: migration 2 only adds the `sessions` table.
The unit test simulates a Phase 1 database (`PRAGMA user_version = 1`, no `sessions` table)
and proves `migrate()` upgrades it. Nothing has to be deleted, and old in-memory sessions
were never persistent anyway.

### Verified

```
make                 # 0 warnings
make test            # 103 checks, 0 failed
sh scripts/smoke.sh  # 32 checks, 0 failed (real server, real restart, real logout-all)
make asan            # clean under AddressSanitizer + UBSan
```

### Still to do

- Failed-login counters are still in memory and per-email only (Phase 4: per IP, in SQLite).
- No screen listing or revoking individual devices yet (only "log out everywhere").
- Email verification, HTTPS, claim verification and the DSA upgrades are later phases.


---

## Phase 3 (this revision): email verification and password reset

Before this phase, anyone could type any address (subject only to the optional
`CC_EMAIL_DOMAIN` suffix check) and the address was never proved. There was also no way
back into an account whose password was forgotten.

### What changed

| File | Change |
|---|---|
| `schema.h` / `schema.cpp` | Schema version 3: `ALTER TABLE users ADD COLUMN email_verified_at`, new `email_tokens` table (single-use verify/reset tokens, indexed), and a backfill that marks existing accounts verified. |
| `secrets.h` / `secrets.cpp` | **New.** `randomToken()` (32 random bytes) and `hashToken()` (SHA-256 hex), shared by sessions and email links. |
| `password.h` / `password.cpp` | **New.** Argon2id hashing moved out of `auth.cpp`, because password reset needs it too. |
| `mailer.h` / `mailer.cpp` | **New.** `Mailer` interface with two implementations: `ConsoleMailer` (prints the email; works offline) and `BrevoMailer` (one HTTPS POST via libcurl, free plan 300/day). `makeMailer()` selects one from `CC_MAIL_MODE`. |
| `email_token_repo.h` / `.cpp` | **New.** Create/lookup/consume single-use tokens, resend cooldown, cleanup. |
| `email_service.h` / `.cpp` | **New.** Verification and reset flows: token TTLs, cooldown, single-use consume, mark verified, new password hash, revoke all sessions. |
| `user_repo.h` / `.cpp` | `User.emailVerifiedAt`; `markVerified()`; `setPasswordHash()`; `email_verified_at` in the row mapping. |
| `auth.h` / `auth.cpp` | Uses `password.h` and `secrets.h`; admin created from server config is marked verified; `normalizeEmail` is public so `/api/forgot` normalises identically. |
| `legacy_json.h` / `.cpp` (importer) | Accounts imported from Part 1 are marked verified, so an upgrade cannot lock existing users out. |
| `main.cpp` | Builds the mailer and EmailService; adds `POST /api/verify`, `/api/verify/resend`, `/api/forgot`, `/api/reset`; includes `emailVerified` in every user JSON; blocks posting for unverified non-admin accounts. |
| `frontend/` | Verify/reset links handled on page load, "Forgot password?" dialog, reset dialog, unverified banner with a resend button, post button disabled until verified. |
| `Makefile` / `Dockerfile` / `.env.example` | New sources, `-lcurl`, `libcurl4-openssl-dev`, mail settings documented. |
| `tests/test_units.cpp` | New `testEmailVerification()` (mailer capture, single use, cooldown, reset, session revocation) and a migration test that upgrades a Phase 1 database to version 3 with the backfill. Now 130 checks. |
| `scripts/smoke.sh` | End-to-end: unverified cannot post, resend cooldown, verify from the link in the server log, single-use, then forgot/reset, old session dead, old password rejected. Now 49 checks. |

### Design decisions

- **One token table, two kinds.** `verify` and `reset` share `email_tokens` with a `kind`
  column. Same lifecycle, same safety rules, half the code.
- **Tokens hashed at rest.** The link holds 256 random bits; the database holds
  `SHA-256(token)`. A leaked `campus.db` cannot be used to verify or reset an account.
- **Single use is enforced in the atomic UPDATE.** `consume()` puts
  `consumed_at IS NULL AND expires_at > ?` inside the `WHERE`; `sqlite3_changes()` decides
  the winner, so two simultaneous clicks cannot both succeed.
- **A reset revokes every session.** If a password leaked, the attacker may already hold a
  session; the reset must end it. Resetting also verifies the account, because opening the
  emailed link proves ownership of the mailbox.
- **No account enumeration.** `/api/forgot` answers identically for a real and an unknown
  address, and the response does not say whether an email was sent.
- **Interface, not a hard-coded provider.** `EmailService` only knows `Mailer&`. The console
  mailer makes the demo offline and deterministic; Brevo is one environment variable away.
- **Synchronous send for now.** Sending happens inside the request, so Brevo adds latency.
  A background queue is listed as a later improvement (and a good DSA/concurrency exercise).

### Visible behaviour changes

1. Registering sends a verification email. Default mode prints it (with the link) to the
   server console.
2. Unverified accounts can browse, but `POST /api/items` answers 403 until they verify.
3. The header shows a banner with a **resend** button while unverified.
4. "Forgot password?" on the login card sends a reset link. Opening it asks for a new
   password; the old sessions are logged out.
5. `GET /api/me`, login and register responses now include `emailVerified`.

### Upgrade path

Migration 3 runs automatically on start. It adds the column and table, then marks every
**existing** account as verified - so upgrading a working Phase 2 install does not stop
anyone from posting. New accounts are unverified. The unit test simulates a Phase 1
database and upgrades it through all three migrations.

### Verified

```
make                 # 0 warnings
make test            # 130 checks, 0 failed
sh scripts/smoke.sh  # 49 checks, 0 failed (real server: verify + reset flows)
make asan            # clean under AddressSanitizer + UBSan
```

### Still to do

- Per-IP rate limiting (Phase 4) and HTTPS (Phase 5).
- Failed-login counters are still in memory and per-email only.
- No "resend" cooldown per IP (only per account), which Phase 4 addresses.
- Real email delivery needs `CC_MAIL_MODE=brevo`, an API key and a verified sender; the
  console mailer is the default so the project works with no accounts or budget.


---

## Phase 4 (this revision): rate limiting per account and per network

The lockout used to be an in-memory `std::unordered_map` keyed only by email. That meant:
a restart cleared every lockout, an attacker could spray thousands of guesses across many
accounts from one machine, and nothing limited sign-ups or reset-request spam.

### What changed

| File | Change |
|---|---|
| `schema.h` / `schema.cpp` | Schema version 4: `rate_limits` table (`key`, `hits`, `window_start`) + index. |
| `rate_limiter.h` / `.cpp` | **New.** Fixed-window counters: `count()`, `record()` (upsert), `clear()`, `prune()`. |
| `auth.h` / `.cpp` | Failed-login counters removed from memory. Limits: 5 per account / 15 min and 20 per network / 15 min. Checked before Argon2 runs. Success clears the account counter, not the IP counter. |
| `main.cpp` | New `clientIp()` helper that honours `X-Forwarded-For` only with `CC_TRUST_PROXY=1`; per-network limits on registration (10/h), verification resend (20/h) and password reset (10/h per network, 3 per address/h). |
| `tests/test_units.cpp` | New `testRateLimiter()`; lockout-persistence and per-IP tests. Now 161 checks. |
| `scripts/smoke.sh` | End-to-end: 5 failures lock the account, a spoofed `X-Forwarded-For` does not bypass it, a spray locks the network, and both limits survive a restart. Now 55 checks. |
| `.env.example`, `README.md` | `CC_TRUST_PROXY` documented; limitations updated. |
| `email_service.cpp`, `frontend/app.js` | Two small polish items from the Phase 3 review: explicit `<utility>` include, and `showApp()` now returns/awaits its render so the "Email verified" message is not immediately overwritten. |

### Design decisions

- **Counters are rows, not memory.** A restart cannot clear an attacker's lockout, and the
  same table serves login, sign-up and reset limits.
- **Two keys, two limits.** `login-email:<address>` (5/15 min) stops password guessing on
  one account; `login-ip:<addr>` (20/15 min) stops one network spraying many accounts.
- **`record()` is an upsert.** `INSERT ... ON CONFLICT(key) DO UPDATE SET hits = hits + 1`
  makes the increment atomic, so two simultaneous requests cannot both start from the
  same count.
- **Check before hashing.** A blocked request costs one indexed SELECT instead of an
  Argon2 run - which is itself a denial-of-service defence.
- **Success forgives the account, not the network.** Otherwise an attacker with one valid
  login could keep resetting the IP counter.
- **`X-Forwarded-For` is opt-in.** Trusting it by default would let any client spoof an IP
  and dodge the limit (or frame another address). `CC_TRUST_PROXY=1` is for a known proxy.
- **The per-email lockout is still grievable.** An attacker can lock a known address by
  failing five times; per-IP limits bound the scale but do not eliminate the attack.
  CAPTCHA or proof-of-work would be the next step. This is documented honestly.

### Visible behaviour changes

1. Six failed logins for one account now get `429` (`Locked`), and the counters survive a
   server restart.
2. Too many failed logins from one network lock that network for 15 minutes, even with the
   correct password.
3. Sign-ups are limited to 10 per network per hour; resets to 10 per network per hour (and
   3 per address), resends to 20 per network per hour.
4. `POST /api/login` returns `429` for both account and network lockouts (same message, so
   the two are not distinguishable to a caller).

### Upgrade path

Migration 4 runs automatically and only adds an empty `rate_limits` table. No data is
touched. Old in-memory counters were never persistent, so there is nothing to migrate.

### Verified

```
make                 # 0 warnings
make test            # 161 checks, 0 failed
sh scripts/smoke.sh  # 55 checks, 0 failed (real server: lockout, spoofing, restart)
make asan            # clean under AddressSanitizer + UBSan
```

### Still to do

- HTTPS (Phase 5): Caddy with a local CA, `CC_COOKIE_SECURE=1`, HSTS.
- Claim verification (Phase 6).
- DSA upgrades: rehashing, top-K instead of re-sorting, inverted index.
- A shared campus NAT can make per-IP limits affect many students at once; tune the
  limits (or run behind a proxy and set `CC_TRUST_PROXY=1`).


---

## Phase 5 (this revision): HTTPS with Caddy

The app still speaks plain HTTP on `127.0.0.1:8080`. A reverse proxy terminates TLS in
front of it - the standard production arrangement, and the reason the C++ code contains
no certificate handling at all.

### What changed

| File | Change |
|---|---|
| `Caddyfile` | **New.** `https://localhost` with `tls internal` for the offline demo, plus the production form (a real domain gets a free Let's Encrypt certificate automatically). |
| `backend/main.cpp` | Optional `Strict-Transport-Security` header when `CC_HSTS=1`. Existing `CC_COOKIE_SECURE=1` already marks the session cookie `Secure`. |
| `.env.example`, `README.md` | HTTPS section, `CC_HSTS`, and the three deployment options (local CA, real domain, Cloudflare Tunnel for a phone demo). |
| `scripts/smoke.sh` | Starts the test server with `CC_HSTS=1` and checks the header is present. |

### Design decisions

- **TLS terminates at the proxy.** Caddy owns certificates, renewal, HTTP/2 and HSTS;
  the app stays a simple HTTP server. This is easier to explain and to operate than
  embedding OpenSSL and certificate files in the binary.
- **`tls internal` for the demo.** No domain is needed, and `sudo caddy trust` installs
  Caddy's local CA so the browser shows a real padlock. For a real deployment, point a
  domain at the machine and Caddy obtains a Let's Encrypt certificate.
- **Secure cookie + HSTS are opt-in.** `CC_COOKIE_SECURE=1` is needed or the browser will
  not send the cookie over HTTPS; `CC_HSTS=1` is dangerous over plain HTTP (a browser
  remembers it for a year), so both are explicit deployment flags.
- **`CC_TRUST_PROXY=1`** must be set when behind the proxy, otherwise every request looks
  like it comes from 127.0.0.1 and the per-IP rate limit is useless.

### What did NOT change

No API, schema or authentication logic changed. The app behaves identically over HTTP and
HTTPS; only two response headers/cookie flags depend on configuration.

### Verified

```
make                 # 0 warnings
make test            # 184 checks, 0 failed
sh scripts/smoke.sh  # 74 checks, 0 failed (includes the HSTS header check)
```

Caddy is not installed on this machine, so the TLS handshake itself was not exercised; the
configuration is standard and the HTTP side (headers, Secure-cookie plumbing) is tested.

---

## Phase 6 (this revision): claim verification for lost & found

Part 1 let the owner (or an admin) mark any notice claimed, with no proof at all. Anyone
could say "that's mine". Now a lost/found notice can carry a **private detail** and a
**private contact**, and a claim has to convince the owner.

### What changed

| File | Change |
|---|---|
| `schema.h` / `schema.cpp` | Schema version 5: `items.proof`, `items.contact`, and a `claims` table (`UNIQUE(item_id, claimant_id)`, status, decided_at/by). |
| `backend/claim_repo.h` / `.cpp` | **New.** Create / list claims, count pending, and `decide()` - one transaction that approves a claim, closes the item and rejects the other pending claims. |
| `backend/item.h`, `item_repo.*` | Notices carry the two private fields; they are loaded from the database but never serialised to other users. |
| `backend/store.*` | `find(id)` (a safe copy for permission checks) and `markClosed(id)` (the approval transaction already closed the row in SQLite; the in-memory index must follow). |
| `backend/main.cpp` | Posting stores `proof`/`contact` for lost/found only; new routes `GET/POST /api/items/:id/claims`, `POST /api/items/:id/claims/:id/decide`, `GET /api/claims/mine`; the item list gains `pendingClaims` for the owner. |
| `frontend/` | Post dialog shows the two private fields for lost/found; "Claims (N)" opens the owner's claim list with Approve/Reject; "This is mine" / "I found this" opens the claim form; "My claims" shows status and the owner's contact after approval. |
| `tests/test_units.cpp`, `scripts/smoke.sh` | Unit and end-to-end coverage for the whole flow, including privacy (no `proof`/`contact` in other users' JSON) and double-approval. |

### The information flow (the part to explain)

```
owner posts lost notice  ── proof (private) ─┐
owner contact (private) ─────────────────────┤  never in GET /api/items
                                             ▼
claimant sends claim: answer + message + own contact
                                             │
owner sees:  answer, message, claimant contact   (only for their own notice)
claimant sees: status "pending", no contact yet  (GET /api/claims/mine)
                                             │
owner approves ──► item closed, other pending claims rejected (one transaction)
claimant sees: status "approved" + owner's contact  (only now)
```

### Design decisions

- **Proof is judged by a human, not the server.** The server cannot know whether "cat
  sticker" is the right answer; the owner can. The server enforces the *process*
  (verified claimant, one claim, open notice, owner decides) and the *disclosure* rules.
- **Approval is one transaction.** Status check inside the `UPDATE`, item closed, other
  pending claims rejected - all in `BEGIN ... COMMIT`. A crash rolls back, so the board
  never ends up with two approved claims or a closed notice with a pending "winner".
- **Private fields stay out of the JSON.** `itemJson()` is the single serialisation point,
  so `proof`/`contact` cannot leak by accident; a smoke test asserts this for another
  user's board.
- **`UNIQUE(item_id, claimant_id)`** is the real "one claim per person" guarantee; the
  friendly check before it is just a nicer error message.
- **Contact exchange is the reward for approval**, which is what makes the feature useful
  without publishing phone numbers on a public board.

### Verified

```
make                 # 0 warnings
make test            # 184 checks, 0 failed
sh scripts/smoke.sh  # 74 checks, 0 failed (full claim flow, privacy, double approval)
make asan            # clean under AddressSanitizer + UBSan
```

### Still to do

- DSA upgrades: rehashing, top-K / ordered index, inverted index.
- Optional: admin dashboard for disputed claims, notifications, background email queue.


---

## Phase 7 (this revision): the DSA upgrades

The data structures were the last items from the original Part 2 plan. They were correct
but had three known weaknesses that an examiner would spot: the hash table never grew, a
page of notices sorted the entire board, and keyword search only matched title prefixes.

### What changed

| File | Change |
|---|---|
| `backend/text.h` / `.cpp` | **New.** The one tokeniser: lowercase, letters/digits only, split on any non-alphanumeric. The trie, the inverted index and the query use it, so a word is indexed exactly as it is searched. |
| `backend/hashtable.h` / `.cpp` | Grows now. `count_` tracks the number of items; when `count / buckets > 0.75` it rehashes into a prime-sized table of roughly double the size. `size()` and `bucketCount()` added for tests. |
| `backend/trie.h` / `.cpp` | Uses the shared tokeniser, and exposes `prefix(cleanedWord)` so `Store` can combine it with the inverted index. |
| `backend/inverted_index.h` / `.cpp` | **New.** `word -> set<id>`, hash-map based, O(1) average per word, with empty buckets removed on erase. |
| `backend/store.h` / `.cpp` | Owns both indexes; `query()` unions trie-prefix and inverted-exact matches per word, ANDs across words, then returns one page using a **bounded top-K heap** ordered worst-first. New `QueryPage { items, total }`. |
| `backend/heap.h` | `size()` added, needed to cap the top-K heap. |
| `backend/main.cpp` | `GET /api/items` takes `limit` (1..100, default 20) and `offset` (>= 0) and returns `{items, total, limit, offset}` instead of a bare array. |
| `frontend/` | Board loads one page at a time, shows "Showing X of Y notices", and has a **Load more** button. Search/filter/sort reset to page 1. |
| `tests/test_units.cpp` | New tests for rehashing (1000 inserts), the inverted index, the tokeniser, and search + pagination (order, totals, page boundaries). Now 224 checks. |
| `scripts/smoke.sh` | Pagination checks (`limit`/`offset`, total, distinct pages, invalid limit). Now 79 checks. |

### Design decisions

- **Rehash at load factor 0.75 into a prime size.** Chains stay short, so lookup stays
  O(1) average. The O(n) move is amortised over the inserts that caused it. Prime sizes
  matter for sequential ids: with a power-of-two bucket count `id % buckets` would send
  every even id to an even bucket.
- **Top-K, not full sort.** To return `[offset, offset+limit)` you only need the first
  `offset+limit` items. A heap ordered worst-first keeps at most that many and pops the
  worst on overflow: O(n log K) time, O(K) memory. The heap's comparator is reversed with
  a lambda (`laterFirst`), so the same `Heap` template serves both directions.
- **Trie + inverted index, not one or the other.** Prefix search (what autocomplete needs)
  and exact-word search (what "wallet" needs) are different queries. `Store::query` unions
  both per word and intersects across words, so "cal", "calculator" and "blue calc" all
  behave the way a user expects.
- **One tokeniser.** When indexing and searching disagree about boundaries, searches fail
  mysteriously. `text.cpp` is the single definition, and it splits on punctuation too, so
  "CALCULATOR-case!" indexes as two words.
- **The API shape had to change.** A bare array cannot tell the client the total or the
  page size. `{items, total, limit, offset}` is explicit, and the frontend uses `total` to
  decide whether to show "Load more".

### Visible behaviour changes

1. `GET /api/items` now returns an object instead of an array, and accepts `limit`/`offset`.
   `limit` outside 1..100, a non-number, or a negative offset is a `400`.
2. The board shows 20 notices at a time with a **Load more** button and a
   "Showing X of Y notices" line.
3. Search still matches prefixes and now also whole words; multi-word queries are ANDed.
4. No database migration: this phase changes only in-memory structures and the API shape.

### Verified

```
make                 # 0 warnings
make test            # 224 checks, 0 failed
sh scripts/smoke.sh  # 79 checks, 0 failed (pagination + search over HTTP)
make asan            # clean under AddressSanitizer + UBSan
```

### Honest limits

- Deep pages get expensive: `K = offset + limit`, so a huge offset approaches the full
  sort. Keyset pagination (page from a "last seen" sort key) is the fix if it ever matters.
- Search indexes titles only (as before); adding descriptions is one more `insert`/`erase`.
- The trie's per-node `std::map<char, Node*>` is memory-heavy compared with a sorted array
  of children or a ternary search tree; fine at this scale, and a good viva discussion.
