# What to study for this revision (Phase 0 + Phase 1)

A study list, in the order that makes the code easiest to defend in a viva. Nothing here
is optional if you want to explain the new files line by line; the "why it matters"
column tells you what question it answers.

Suggested order: **A → B → C → D → E → F → G**. Roughly 6-9 hours total if SQLite is new to
you. Do the two `sqlite3` practice sessions, they are the highest-value part.

---

## A. SQL and the relational model (the big one)

| Topic | Why it matters here | Where to see it |
|---|---|---|
| Tables, columns, data types | The whole storage layer | `schema.h` |
| `PRIMARY KEY` | Every row's unique name; `id` in both tables | `schema.h` |
| `AUTOINCREMENT` | Why the database assigns notice ids, not C++ | `user_repo.h`, `item_repo.h` |
| `UNIQUE` constraint | What really makes "one account per email" true under concurrency | `users.email` |
| `CHECK` constraint | Why `role IN ('user','admin')` cannot be violated by a bug | `schema.h` |
| `NOT NULL`, `DEFAULT` | Why a missing field cannot silently become NULL | `schema.h` |
| **Foreign key** + `ON DELETE CASCADE` | A notice cannot exist without its author | `items.owner_id` |
| **JOIN** | Why the notice stores only `owner_id` but the UI shows a name | `item_repo.h::loadAll()` |
| **Index** (B-tree) | Why sorting by date/price does not scan every row | `idx_items_status_created`, `idx_items_status_price` |
| **Prepared statement** + bound parameters | The actual answer to "how do you stop SQL injection?" | every query in `*_repo.h` |
| **Transaction** (`BEGIN` / `COMMIT` / `ROLLBACK`) | Why "all or nothing" matters when upgrading or importing | `db.h::Transaction`, `schema.h::migrate()` |
| `COUNT(*)`, `UPDATE ... WHERE`, `DELETE ... WHERE` | The repository methods | `user_repo.h`, `item_repo.h` |
| Normalisation (store facts once) | Why the owner's name is not copied into items | `item_repo.h` |

Practice (15 minutes, worth it): open a terminal, `sqlite3 /tmp/practice.db`, then paste the
`SCHEMA_SQL` string from `backend/schema.h`, insert two users and two notices, and run:

```sql
.schema
SELECT * FROM items;
SELECT i.title, u.name FROM items i JOIN users u ON u.id = i.owner_id;
EXPLAIN QUERY PLAN SELECT * FROM items WHERE status = 'open' ORDER BY created_at DESC;
```

That last one shows the index being used. That is the demonstration to give a panel.

---

## B. SQLite specifically (small, but asked about)

| Topic | Why it matters | Where |
|---|---|---|
| One file = one database | Backup, reset, "where is my data?" | `backend/data/campus.db` |
| **WAL journal mode** | Readers do not block the writer; crash-safe commits | `db.cpp` constructor |
| **`PRAGMA`** statements | Configuration: `foreign_keys`, `synchronous`, `journal_mode` | `db.cpp` |
| **`PRAGMA user_version`** | How migrations know what the file has already seen | `schema.h` / `schema.cpp` |
| **`sqlite3_prepare_v2` / `step` / `finalize`** | The C API under the wrapper | `db.cpp` |
| `sqlite3_bind_*` and `SQLITE_TRANSIENT` | Why the string can be freed after binding | `db.cpp` |
| `sqlite3_last_insert_rowid` | Where new notice ids come from | `db.cpp`, `item_repo.cpp` |
| `sqlite3_changes` | "Did that UPDATE actually touch a row?" | `item_repo.cpp` |
| **`busy_timeout`** | What happens if another writer holds the lock | `db.cpp` |
| Single connection + **serialized threading mode** | Is it safe to share one `sqlite3*` across httplib's threads? | comment at top of `db.cpp` |
| `VACUUM INTO` | One-line backup answer | README |
| Why not a server DB (MySQL/Postgres) | Cost, setup, offline demo, and "the data model is relational but the deployment is one process" | README / change notes |
| `ALTER TABLE ... ADD COLUMN` + backfill | Migrating a live table: add `email_verified_at`, then mark existing rows verified | `schema.cpp` |
| Single-use tokens | `consumed_at` + the conditions inside the `UPDATE`, so two clicks cannot both succeed | `email_token_repo.cpp` |
| `rate_limits` table | One row per counter: `key`, `hits`, `window_start` | `schema.cpp` |
| **Upsert** (`ON CONFLICT ... DO UPDATE`) | Insert the first hit or increment atomically in one statement | `rate_limiter.cpp` |
| `claims` table | One row per claimant; `UNIQUE(item_id, claimant_id)` is the real "one claim each" guarantee | `claim_repo.cpp` |

### Sessions (Phase 2)

- The browser holds a random 256-bit **token** in the cookie; the database stores only its
  **SHA-256 hash**. A stolen database file therefore cannot be replayed as a login.
- Passwords use **Argon2id** (slow, salted) because they are guessable; tokens use
  **SHA-256** (fast) because they are already random. Know why the two differ.
- Expiry is enforced in the lookup query (`expires_at > ?`), so an expired row is refused
  even before cleanup deletes it.
- `logout` = delete one row by token hash; `logout-all` = delete every row for the user.
- Indexes on `user_id` and `expires_at` keep those deletes cheap.
- The trim statement keeps only the newest 10 rows per account, using a subquery
  (`token_hash NOT IN (SELECT ... ORDER BY created_at DESC LIMIT ?)`).

### Email verification and reset (Phase 3)

- Registering creates a `verify` token; "Forgot password?" creates a `reset` token.
  Same table, different `kind`.
- The link contains a 256-bit random token; the database stores only its SHA-256 hash.
- Single-use: `consume()` is `UPDATE ... WHERE consumed_at IS NULL AND expires_at > ?`,
  so two simultaneous clicks cannot both succeed (`sqlite3_changes()` decides the winner).
- Expiry: verify 24 h, reset 1 h. Re-send cooldown: 60 s per kind.
- A successful reset changes the password hash and **deletes every session** for the user.
- `/api/forgot` always answers the same way, so it cannot be used to discover accounts.

### Rate limiting (Phase 4)

- Fixed-window counters in SQLite: `key` (+ `hits`, `window_start`). Keys look like
  `login-email:someone@x`, `login-ip:203.0.113.7`, `register-ip:...`, `forgot-ip:...`.
- Limits: 5 failed logins per account / 15 min, 20 per network / 15 min, 10 sign-ups per
  network / hour, plus bounded reset and resend requests.
- The check happens **before** Argon2 runs, so a locked-out caller costs one indexed SELECT.
- `record()` is an upsert, so there is no read-then-write race between two requests.
- A success clears the account counter but not the IP counter: an attacker who knows one
  password must not be able to reset the network counter by logging in.
- Counters survive restarts because they are rows, not a `std::map` in memory.
- `X-Forwarded-For` is trusted only with `CC_TRUST_PROXY=1`; otherwise any client could
  send the header and dodge the per-IP limit.

### Claims for lost & found (Phase 6)

- An `items` row can hold `proof` (a private detail only the real owner knows) and
  `contact` (private). Neither is in `itemJson()`, so neither can leak through the API.
- A claim stores the claimant's answer, message, contact and status
  (`pending` / `approved` / `rejected`); one claim per person per notice.
- `decide()` is **one transaction**: flip the claim, close the item, reject the other
  pending claims. The status check is inside the `UPDATE`, so two approvals cannot both win.
- Information flow: owner sees every claim on their notice; a claimant sees only their own
  claim status, and the owner's contact only after approval.
- The server cannot judge the answer - only the owner can. The server enforces the process
  (verified claimant, open lost/found notice, owner decision) and the disclosure rules.

Two facts to be able to state: SQLite uses a **B+ tree** for its indexes (so range and
`ORDER BY` queries are cheap), and it is **embedded** (links into your process; no separate
server, no port, no account).

---

## C. C++ concepts used in the new code

| Concept | Where | What to say |
|---|---|---|
| **RAII** (constructor acquires, destructor releases) | `Database`, `Statement`, `Transaction` | "The destructor is the cleanup path, so there is no way to forget it, even when an exception is thrown." |
| **Destructor does the right thing on the error path** | `Transaction::~Transaction()` | Rolls back unless `commit()` was called |
| **`= delete` on copy** | `Statement`, `Database`, `Transaction` | Two owners of one `sqlite3_stmt*` would double-free |
| **Reference member + dependency injection** | `UserRepo(Database&)`, `Store(ItemRepo&)` | The classes do not create their own database; the caller decides. This is what makes `:memory:` testing possible |
| **Exceptions for unexpected failures, error codes for expected ones** | `DbError` vs `"an account with this email already exists"` | A DB failure is a bug/incident; a duplicate email is a normal answer |
| **Overloading** | `Statement::bind(int, int/long long/double/string/const char*)` | Same idea, typed binding |
| **Declaration / definition split** | every `.h` + `.cpp` pair | Headers declare, exactly one `.cpp` defines; see section D |
| **Virtual functions / polymorphism** | `Mailer`, `ConsoleMailer`, `BrevoMailer` | `EmailService` calls `Mailer&` and never knows which implementation it got |
| **`std::unique_ptr` + `make_unique`** | `makeMailer()` | The factory owns the chosen mailer; no `delete`, no leaks |
| **`override`** | `ConsoleMailer::send`, `BrevoMailer::send` | The compiler checks the signature really matches the interface |
| **`std::optional`** | `UserRepo::findById` | "Not found" is not an error, and there is no null pointer to forget |
| **`std::function` comparator** | `Heap<T>` | The container does not know what "first" means; the caller passes it |
| **Lambdas** | heap comparators, httplib handlers | Behaviour passed as data |
| **`std::filesystem`** | `main.cpp`, `legacy_json.h` | Create the data directory, remove temp dirs in tests |
| **`const` correctness** | `Statement` column getters, comparators | Reading does not modify |
| **Templates** | `Heap<T>` | One implementation for `Item` and for `int` (tests) |
| **`std::mutex` + `lock_guard`** | `Store`, `Auth` | httplib runs handlers concurrently; the lock is released automatically |
| **Move semantics** | *not used (deliberately)* | Say honestly: "copying is disabled rather than moved, because these classes are small and never need to move" |
| **`-Wall -Wextra` and sanitizers** | `Makefile` | `make asan` under AddressSanitizer + UBSan |

---

## D. Headers vs .cpp files (added in this revision)

| Topic | Why it matters | Where to see it |
|---|---|---|
| **Translation unit** | Every `.cpp` plus the headers it includes is compiled on its own into one `.o` file | `Makefile`, `build/` |
| **Declaration vs definition** | A header says *what* exists; a `.cpp` supplies the body | `db.h` vs `db.cpp` |
| `#pragma once` | Include guard: the same header is read once per translation unit | every header |
| **One Definition Rule (ODR)** | A function may be defined only once in the whole program; header-only implementations risk duplicate symbols | the reason for the split |
| **Linker** | Joins `main.o` + `db.o` + ... into the executable; "undefined reference" means a missing `.cpp` | `Makefile` link line |
| **Forward declaration** | `class Database;` is enough to declare a function that takes `Database&` | `legacy_json.h` |
| **Include what you use** | `auth.h` no longer includes `sodium.h`; only `auth.cpp` needs it | `auth.h`, `auth.cpp` |
| **Templates must be in headers** | `Heap<T>` does not exist until instantiated, so the compiler needs the full definition | `heap.h` |
| `inline constexpr` | A header-safe constant that can be used from several `.cpp` files | `SCHEMA_VERSION` |
| Anonymous `namespace { }` | Keeps helper functions local to one `.cpp` (internal linkage) | `auth.cpp`, `trie.cpp`, `*.cpp` |
| **Compile time** | Editing one `.cpp` rebuilds one object, not the whole program | `make` output |

Exercise worth doing once:

```bash
touch backend/store.cpp && make    # only store.o + the link run
make clean && time make            # full rebuild
```

That difference is the entire point of the split, and it is the answer to "why
not just put everything in headers?".

## E. Architecture / design ideas

| Idea | Where | One-line explanation for the viva |
|---|---|---|
| **Layering** | handlers → Store/Auth → repos → SQLite | Each layer only talks to the one below it |
| **Repository pattern** | `user_repo.h`, `item_repo.h` | One class owns the SQL for one table, so queries are never scattered |
| **Source of truth vs cache** | database vs `HashTable`/`Trie`/`Heap` | The database is the truth; memory is a read cache rebuilt at start-up |
| **Write-through order** | `Store::add/close/remove` | Save to the database first, then update memory, so a crash never leaves memory claiming something the disk does not have |
| **Index rebuild at start-up** | `Store::Store(ItemRepo&)` | O(n) once, instead of re-reading and re-parsing a file on every request |
| **Migration/versioning** | `PRAGMA user_version`, `migrate()` | A new release upgrades existing installs in place |
| **Idempotent start-up** | `migrate()`, `importLegacyJson()`, `ensureAdmin()` | Calling it twice is harmless; starting twice never duplicates data |
| **Server-side sessions vs stateless tokens** | `session_repo.h` | Rows can be revoked (logout-all); a self-contained JWT cannot be revoked without a denylist. |
| **Rehashing / load factor** | `hashtable.cpp` | Grow at 0.75 load into a prime-sized table; O(n) occasionally, O(1) per insert amortised. |
| **Top-K heap** | `store.cpp` | Keep only `offset + limit` best items: O(n log K) time and O(K) memory instead of a full sort. |
| **Inverted index + trie** | `inverted_index.cpp`, `trie.cpp` | Exact words vs prefixes; `Store` unions per word and ANDs across words. |
| **Shared tokenisation** | `text.cpp` | Indexing and searching must agree on word boundaries, or searches fail mysteriously. |
| **Claim decision as one transaction** | `claim_repo.cpp` | Approve + close item + reject the other pending claims happen together, or not at all. |
| **Fixed-window rate limiting** | `rate_limiter.h` | `count()` decides, `record()` increments, `clear()` forgives. Simple, queryable, survives restarts. |
| **Interface + factory** | `mailer.h`, `email_service.h` | `EmailService` depends on `Mailer&`; `makeMailer()` reads configuration and picks console vs Brevo. |
| **Fail closed** | `Store` throws before memory changes | An error leaves the system consistent |
| **Never leak internals** | `main.cpp` exception handler | The real error is logged; the browser gets a generic 500 |

---

## F. Security lessons this changes

1. **SQL injection** is prevented by *prepared statements with bound parameters*, not by
   "escaping quotes". The value is never parsed as SQL. Point at any `prepare(...)`.
2. **A check before an insert is not a guarantee**; the `UNIQUE` constraint is. Two
   requests can pass the same `SELECT` at the same time.
3. **Constraints are a second line of defence.** Validate in C++ for a friendly message;
   let the database refuse the impossible.
4. **Least privilege for files.** `campus.db` contains password hashes; `.gitignore` has
   it, and it must never be committed. (Add `-wal`/`-shm` too, as `.gitignore` does.)
5. **Error messages are information.** Sending `sqlite3_errmsg` to the browser tells an
   attacker table names and paths; log it instead.
6. **Atomicity protects integrity.** A crash during a transaction leaves the previous
   state, not a half-written one - the same reason Part 1 used temp-file + rename.
7. **Session tokens are secrets too.** Hashing them at rest means a leaked database file
   cannot be replayed as a login, and deleting the row is a real logout - which is why
   server-side sessions beat self-contained tokens for a system that needs revocation.
8. **Single-use means atomic.** The conditions live in the `UPDATE`, not in a separate
   `SELECT` beforehand; that is what makes a double-click safe.
9. **Do not confirm whether an account exists.** `/api/forgot` gives the same answer for a
   real and an unknown address, so it cannot be used to enumerate users.
10. **Never trust `X-Forwarded-For` by default.** Any client can send it; it is only
    believable when your own proxy sets it. Rate limiting is a tradeoff too: a campus
    network shares one public IP, so per-IP limits must be tuned, not set arbitrarily low.
11. **Minimise disclosure: private data never reaches the serialiser.** `proof`/`contact`
    live only in the database and in the owner's own views; `itemJson()` is the single
    place a notice becomes JSON, so the rule is enforceable and testable.
12. **TLS belongs at the edge.** Terminate HTTPS in Caddy, send `Secure` cookies and HSTS
    from the app, and keep certificates out of the C++ code. HSTS must only be sent over
    HTTPS - it is remembered by the browser for a year.

---

## G. Build and tooling

| Topic | Where | What it does |
|---|---|---|
| `make` targets and prerequisites | `Makefile` | Rebuilds only when a listed source is newer |
| `pkg-config` | `Makefile` | Finds Homebrew's libsodium include/lib dirs |
| `curl -fsSL` + SHA-256 checks | `scripts/fetch_deps.sh`, `Dockerfile` | Supply-chain safety: a changed download fails the build |
| Sanitizers | `make asan` | Catches use-after-free, leaks, undefined behaviour |
| Caddy reverse proxy | `Caddyfile` | TLS termination; `tls internal` for a trusted local demo, Let's Encrypt for a real domain |
| `set -e`, `trap`, `mktemp` | `scripts/smoke.sh` | Shell script that cleans up after itself and stops on the first error |
| `exec` in a background subshell | `scripts/smoke.sh` | Why killing the wrapper and leaving the server running was a bug we fixed |
| Docker multi-stage-ish layout, non-root user | `Dockerfile` | The container runs as `campus`, not root |

---

## H. File-by-file: be able to say what it does in one sentence

| File | One sentence |
|---|---|
| `db.h` / `db.cpp` | Declaration and implementation of the RAII SQLite wrapper. |
| `schema.h` / `schema.cpp` | Declares `migrate()`; the SQL table definitions live in the `.cpp`. |
| `user_repo.h` / `user_repo.cpp` | Every SQL statement that touches accounts, and the `User` record. |
| `item_repo.h` / `item_repo.cpp` | Every SQL statement that touches notices, including the owner JOIN. |
| `session_repo.h` / `session_repo.cpp` | Login sessions: create, look up by token hash, revoke one / all, prune expired. |
| `secrets.h` / `secrets.cpp` | Random tokens (`randomToken`) and SHA-256 hashing (`hashToken`). |
| `password.h` / `password.cpp` | Argon2id hashing and verification, shared by Auth and reset. |
| `mailer.h` / `mailer.cpp` | `Mailer` interface + console and Brevo implementations + factory. |
| `email_token_repo.h` / `.cpp` | Single-use verify/reset tokens in SQLite. |
| `email_service.h` / `email_service.cpp` | Verification and password-reset flows. |
| `rate_limiter.h` / `rate_limiter.cpp` | Fixed-window counters in SQLite, shared by login, sign-up and reset. |
| `claim_repo.h` / `claim_repo.cpp` | Lost & found claims: create, list, and the approve/reject transaction. |
| `Caddyfile` | Reverse-proxy config that terminates TLS in front of the app. |
| `legacy_json.h` / `legacy_json.cpp` | A one-time import that copies Part 1's JSON files into the database. |
| `store.h` / `store.cpp` | The in-memory indexes (hash table + trie + inverted index + top-K heap) over notices, writing to the database first. |
| `auth.h` / `auth.cpp` | Password hashing, sessions, lockout and roles, backed by `UserRepo`. |
| `hashtable.h` / `hashtable.cpp` | Chaining hash table: id -> notice, rehashes into a bigger prime table at load 0.75. |
| `trie.h` / `trie.cpp` | Prefix tree over title words (`prefix()` + multi-word `search()`). |
| `inverted_index.h` / `.cpp` | Word -> notice ids, for exact-word keyword search. |
| `text.h` / `text.cpp` | The one tokeniser: lowercase, alphanumeric, split on punctuation. |
| `heap.h` | Template binary heap. Stays header-only because a template must be visible where it is used. |
| `item.h` | The `Item` struct. Stays header-only: a shared data type has no implementation to move. |
| `main.cpp` | Wiring: open the database, migrate, import, build indexes, define the HTTP routes. |
| `Makefile` | Compiles each `.cpp` to `build/*.o`, links, tests, sanitizer build. |
| `tests/test_units.cpp` | 224 assertions that prove the DSA, database, auth, sessions, email, rate limits, claims, search, pagination and import work. |
| `scripts/smoke.sh` | 79 checks against the real server, including restart, verification, reset, lockout, claims and pagination. |

---

## I. Viva questions and short answers

1. **Why a database instead of JSON files?** Transactions, constraints, indexes,
   concurrency, and versioned migrations; the JSON file was rewritten in full on every
   change and could be corrupted by a crash.
2. **Why SQLite and not MySQL/Postgres?** Free, embedded, no server or account to manage,
   works offline in the demo, and the data model is still properly relational.
3. **What is a prepared statement?** SQL compiled once with `?` holes; values are bound
   as data, never parsed as SQL, so injection cannot happen.
4. **Why keep the HashTable/Trie/Heap at all?** The database is durability and ordered
   range scans; the in-memory structures are O(1) lookup and prefix search and are part of
   the coursework. A pure-SQL version would use `CREATE INDEX`.
5. **What happens if the server crashes right after saving a notice?** The commit either
   happened or did not; after restart the index is rebuilt from the database, so the
   acknowledged write is present.
6. **Why save to the database before updating memory?** So an error or crash cannot leave
   memory claiming something the disk never received.
7. **How do you handle two requests registering the same email?** The C++ check gives a
   friendly error; the `UNIQUE` constraint is the real guarantee.
8. **What does `PRAGMA user_version` do?** Stores the schema version in the database
   header, so `migrate()` runs only steps the file has not seen.
9. **Why is the owner's name not stored in `items`?** Normalisation: one fact, one place;
   `loadAll()` joins it from `users`.
10. **Is one SQLite connection safe across httplib's threads?** Yes in SQLite's default
    serialized mode; write transactions are short and the program-level lock serialises
    the paths that matter. At larger scale, one connection per thread or a pool.
11. **How do you know the migration did not lose data?** `make test` covers repositories
    and restart; `scripts/smoke.sh` imports real legacy JSON, restarts the server and
    checks the notices are still there.
12. **Why did you split headers and sources?** Declarations in the `.h`, definitions in one
    `.cpp`: the One Definition Rule is respected, editing one file recompiles one object, and
    headers stop leaking includes such as `sodium.h`.
13. **Which files must stay header-only, and why?** `heap.h` because it is a template (the
    compiler needs the full definition where `Heap<Item>` is instantiated); `item.h` because it
    is a plain struct with no code.
14. **What is a translation unit, and what does the linker do?** Each `.cpp` plus its includes is
    compiled into an object file; the linker joins the objects and resolves symbols such as
    `migrate` and `Database::prepare`.
15. **What happens if the same function is defined in two `.cpp` files?** A multiple-definition
    linker error (ODR violation). If no `.cpp` defines it: an undefined-reference error.
16. **Why store a hash of the session token instead of the token itself?** A database
    backup or a stolen disk would otherwise be enough to log in as every active user. The
    token already has 256 random bits, so SHA-256 is enough - unlike a password, which
    needs slow, salted Argon2id.
17. **Why not a JWT?** A self-contained token cannot be revoked without a denylist, and a
    denylist is the sessions table with extra steps. Server-side rows make logout,
    logout-all and expiry trivial and easier to explain.
18. **How does "log out everywhere" work?** `DELETE FROM sessions WHERE user_id = ?`;
    every device's token stops matching on its next request.
19. **What happens to sessions when the server restarts now?** They survive - they are rows
    in SQLite. Only the in-memory failed-login counters are lost until Phase 4 moves them.
20. **How is expiry enforced, and why does the table not grow forever?** The lookup requires
    `expires_at > now`, so expired rows are refused before cleanup; expired rows are deleted
    at start-up and on each login; and each account keeps at most 10 sessions.
21. **Why is `Mailer` an interface?** So the demo can run offline with the console mailer,
    production can use Brevo, and tests can capture messages - without EmailService changing.
    It is also the cleanest example of polymorphism in the project.
22. **Why is the console mailer the default?** Deterministic, free and offline: the link is
    printed where the demo can see it. Real delivery is then configuration (`CC_MAIL_MODE=brevo`).
23. **How do you stop someone verifying or resetting an account they do not own?** The token
    exists only in the emailed link; the database holds a hash; it is single-use, expiring,
    and a reset also revokes every session.
24. **Why does `/api/forgot` always return the same message?** Otherwise it would tell an
    attacker which email addresses have accounts (user enumeration).
25. **Why revoke all sessions on a password reset?** If the old password leaked, the attacker
    may already have a session; the reset must end it, and it also proves email ownership.
26. **Why are the rate-limit counters in SQLite and not in memory?** They survive a restart,
    they are shared by several endpoints, and a crash cannot reset an attacker's lockout.
27. **Fixed window or sliding window?** Fixed window is the simplest correct-enough choice:
    one row per key and one upsert. Its weakness is a burst at the window boundary (double
    the limit in a short span); a sliding window or token bucket fixes that at more cost.
28. **Why limit per IP as well as per account?** A per-account lockout can be abused to lock
    a known user, and it does nothing against an attacker spraying many accounts from one
    network. Per-IP bounds the spray; both together bound both attacks, imperfectly.
29. **Why is `X-Forwarded-For` dangerous?** A client can send any value; believing it by
    default lets them dodge the per-IP limit (and frame someone else's IP). It is trusted
    only with `CC_TRUST_PROXY=1`, and the proxy must be the one setting it.
30. **Why clear the account counter on success but not the IP counter?** Otherwise someone
    who knows one valid password could reset the network counter at will; successes should
    not make a network look safer than it is.
31. **Why terminate TLS in Caddy instead of the C++ process?** Certificates, renewal,
    HTTP/2 and HSTS become configuration instead of code; the app stays a plain HTTP
    server on loopback. It also matches how real deployments are built.
32. **Why must HSTS only be sent over HTTPS?** The browser remembers it for a year; sent
    over plain HTTP it would lock users out of a working HTTP site.
33. **What does each cookie flag do?** `HttpOnly` hides the token from JavaScript (XSS
    theft), `Secure` stops it travelling over HTTP, `SameSite=Strict` stops cross-site
    CSRF. TLS protects the token in transit; the flags protect it in the browser.
34. **What stops a random person claiming a lost item?** They need a verified account and
    a plausible answer to a private detail; the owner judges, and contacts are exchanged
    only after approval. One claim per person per notice, one approval per notice.
35. **Why approve inside a transaction?** The item must close, one claim must win and the
    rest must lose together; otherwise two people could be sent to the same handover.
36. **Why are `proof` and `contact` not in the JSON?** Data minimisation: nobody but the
    owner (and, for contact, the approved claimant) needs them. Because `itemJson()` is
    the only serialisation point, the rule is easy to enforce and to test.
37. **Why rehash?** With a fixed bucket count, chain length grows as notices/buckets. At
    1000 notices in 101 buckets that is ~10 per chain, so "O(1)" degrades towards O(n).
    Growing at load factor 0.75 keeps chains short; the O(n) move is amortised.
38. **Why double to a prime, not a power of two?** `id % buckets` is the hash. A
    power-of-two bucket count maps every even id to an even bucket; a prime spreads
    sequential ids much better. (A real hash like FNV would also help.)
39. **Why a top-K heap instead of a full sort?** A page only needs the first
    `offset + limit` items in order. Keeping those in a worst-first heap is O(n log K)
    time and O(K) memory, versus O(n log n) and O(n) for sorting everything.
40. **Why keep both a trie and an inverted index?** They answer different questions:
    prefix ("cal" -> calculator) vs exact word ("wallet"). `Store::query` unions them per
    word and ANDs across words.
41. **Why one tokeniser?** If indexing and searching disagree on word boundaries, a word
    that is indexed can be unsearchable. `text.cpp` defines it once.
42. **What are the limits of top-K pagination?** Deep pages make K large, approaching a
    full sort; keyset pagination is the scalable alternative. Also a page still scans all
    matching items to count the total.
43. **What would you change next?** Optional extras: an admin dashboard for disputes and
    stats, in-app notifications or an SSE live board, a background email queue, item
    images, keyset pagination, indexing descriptions as well as titles.
