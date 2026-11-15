# CampusConnect (Part 1 + Part 2)

A campus noticeboard for selling, lending, and lost & found items.
Frontend: HTML, CSS, JavaScript. Backend: C++17 with hand-written DSA, plus a SQLite database.

Part 1 is the working product: accounts with roles, notices, search, sorting, moderation.
Part 2 adds accountability and safety: sessions, email verification, rate limiting, HTTPS and
claim verification are all done (Phases 2-6). What remains is the DSA upgrade list at the end.

## What changed in this revision (Phases 0-7 + header/source split)

Part 1 used to save everything to two JSON files (`items.json`, `users.json`) that were
rewritten in full on every change. Storage is now a real database:

- SQLite database at `backend/data/campus.db`, with a schema, constraints, indexes,
  prepared statements, transactions and versioned migrations.
- Accounts and notices go through small "repository" classes that own the SQL.
- The existing JSON files are imported automatically, once, on first start.
- The DSA structures are still there, now as an in-memory index rebuilt from the
  database at start-up: the database is the truth, the indexes are the speed.
- `make` builds, `make test` runs 224 unit checks, `scripts/smoke.sh` runs 79
  end-to-end checks against the real server, including a restart with a live session.

- Header-only code was split into declarations (`.h`) and definitions (`.cpp`), so editing
  one file recompiles one object instead of the whole program.
- **Phase 2:** login sessions live in SQLite, so restarting the server no longer logs
  everyone out. The database stores only a SHA-256 hash of each token, and a new
  **Log out all** button (`POST /api/logout-all`) ends every session for the account.
- **Phase 3:** email verification and password reset. Registering sends a single-use
  link (printed by the console mailer, or sent through Brevo); unverified accounts can
  browse but not post. "Forgot password?" sends a reset link that also signs you out
  everywhere. `Mailer` is an interface: delivery is configuration, not code.
- **Phase 4:** rate limiting. Failed logins are limited per account (5 / 15 min) **and** per
  network (20 / 15 min), and the counters are rows in SQLite, so a restart cannot clear a
  lockout. Sign-ups, verification resends and reset requests are limited per network too.
  `X-Forwarded-For` is believed only when `CC_TRUST_PROXY=1`, so it cannot be spoofed.
- **Phase 5:** HTTPS deployment. A `Caddyfile` puts a TLS-terminating reverse proxy in front
  of the app (`tls internal` for a trusted local demo, Let's Encrypt for a real domain),
  with `CC_COOKIE_SECURE=1` for the cookie and optional `CC_HSTS=1`. No application code
  handles certificates.
- **Phase 6:** claim verification for lost & found. The poster writes a private detail only
  the real owner would know; other users send claims with an answer and a contact method;
  the owner (or an admin) approves one, which closes the notice, rejects the other claims,
  and only then reveals the owner's contact to the winning claimant.
- **Phase 7:** the DSA upgrades. The hash table now **rehashes** when it fills, the board is
  **paginated with a bounded top-K heap** instead of sorting everything on every request, and
  an **inverted index** (exact words) works alongside the trie (prefixes). A shared tokeniser
  makes indexing and searching agree.

Details and reasoning: [docs/CHANGES.md](docs/CHANGES.md).
Study list for the viva: [docs/STUDY-TOPICS.md](docs/STUDY-TOPICS.md).

## Run

1. Install libsodium (password hashing). SQLite is part of macOS, so nothing to install:
   `brew install libsodium`
2. Build. This also downloads the two third-party headers (`httplib.h`, `json.hpp`)
   once, and verifies their SHA-256 checksums:
   ```
   make
   ```
3. Start it with an admin account (only needed the first time; the admin is saved):
   ```
   CC_ADMIN_EMAIL=admin@geu.ac.in CC_ADMIN_NAME="Lost & Found Desk" \
   CC_ADMIN_PASSWORD='choose-a-long-password' make run
   ```
4. Open http://localhost:8080, create a student account, and log in.

Always open the site through the server (http://localhost:8080), not by double-clicking
`index.html`. There is no sample or demo data.

To start over with an empty board, stop the server and delete `backend/data`
(this also deletes all accounts).

With Docker: `docker build -t campusconnect . && docker run -p 8080:8080 -v campusdata:/app/backend/data -e CC_ADMIN_EMAIL=... -e CC_ADMIN_PASSWORD=... campusconnect`

## HTTPS (Phase 5)

The C++ server speaks plain HTTP on `127.0.0.1:8080`; a reverse proxy terminates TLS in
front of it. That is the normal production arrangement - the application never touches
certificates or renewals.

Local demo with a trusted certificate (no domain, no cost):

```bash
brew install caddy
sudo caddy trust      # once: installs Caddy's local CA in the system trust store
caddy run             # reads ./Caddyfile, serves https://localhost
CC_COOKIE_SECURE=1 CC_HSTS=1 CC_BASE_URL=https://localhost make run
```

Open https://localhost. The same `Caddyfile` shows the production form: point a real domain
at the machine (ports 80/443 reachable) and Caddy obtains a free Let's Encrypt certificate
automatically.

Phone demo without a domain: `cloudflared tunnel --url http://localhost:8080` prints a free
`https://…` URL; start the app with `CC_BASE_URL=<that URL>` so emailed links point there.

Only set `CC_COOKIE_SECURE=1` and `CC_HSTS=1` when the request really is HTTPS (a `Secure`
cookie is not sent over plain HTTP, and HSTS would lock users out of an HTTP site). Behind
the proxy, set `CC_TRUST_PROXY=1` so the per-IP rate limits see real client addresses.

### Settings (environment variables)

| Variable | Meaning | Default |
|---|---|---|
| `CC_ADMIN_EMAIL`, `CC_ADMIN_NAME`, `CC_ADMIN_PASSWORD` | Creates the first admin if no admin exists. Ignored afterwards. | none |
| `CC_EMAIL_DOMAIN` | Only allow sign-up with this email domain, e.g. `geu.ac.in` | any domain |
| `CC_DATA_DIR` | Folder holding `campus.db` (and any old JSON files) | `data` |
| `CC_FRONTEND_DIR` | Folder served as the website | `../frontend` |
| `CC_PORT` | TCP port to listen on | `8080` |
| `CC_COOKIE_SECURE` | Set to `1` when served over HTTPS | off |
| `CC_MAIL_MODE` | `console` prints emails (offline demo); `brevo` sends real email | `console` |
| `CC_BREVO_API_KEY` | Brevo API key when `CC_MAIL_MODE=brevo` | none |
| `CC_BASE_URL` | Base URL used inside emailed links | `http://localhost:<port>` |
| `CC_MAIL_FROM`, `CC_MAIL_FROM_NAME` | Sender identity in emails. For Brevo, the sender address must be verified in Brevo or covered by an authenticated domain. | local defaults |
| `CC_TRUST_PROXY` | Set to `1` only behind a proxy you control, so `X-Forwarded-For` is trusted for per-IP limits | off |
| `CC_HSTS` | Set to `1` when served over HTTPS, to send Strict-Transport-Security | off |

Copy `.env.example` if you prefer to keep these in a file (remember to `export` them).

## Database

The whole database is one file: `backend/data/campus.db` (+ `-wal` / `-shm` files while
the server runs). Back it up by copying the file, or with
`sqlite3 campus.db "VACUUM INTO 'backup.db'"`.

Tables (see `backend/schema.h`):

| Table | Holds |
|---|---|
| `users` | id, email (unique), name, Argon2id password hash, role, created_at, email_verified_at |
| `items` | id, owner_id → users, type, title, description, category, location, price, status, created_at, proof (private), contact (private) |
| `sessions` | token_hash (SHA-256 of the cookie token), user_id → users, created_at, expires_at |
| `email_tokens` | token_hash, user_id → users, kind (`verify`/`reset`), created_at, expires_at, consumed_at |
| `rate_limits` | key (`login-email:…`, `login-ip:…`, …), hits, window_start |
| `claims` | id, item_id → items, claimant_id → users, answer, message, contact, status, created_at, decided_at, decided_by |

Why a database instead of JSON files:

- **Transactions.** A write either fully happens or does not happen; a crash mid-write
  cannot leave a half-written file.
- **Constraints.** `UNIQUE(email)`, `CHECK(role IN ...)`, foreign keys: the database
  refuses impossible data even if the C++ validation has a bug.
- **Indexes.** Sorting by date or price no longer means sorting every row on every request.
- **Concurrency.** No need to rewrite every notice to add one notice.
- **Migrations.** `PRAGMA user_version` records the schema version; new releases upgrade
  an existing database in place.

### Migrations

`backend/schema.h` / `schema.cpp` hold the DDL and a `migrate(Database&)` function. It runs
only the steps the file has not seen yet: migration 2 added sessions, migration 3 added
email verification and single-use tokens, migration 4 added the rate-limit counters, and
migration 5 added claim verification. Old databases are upgraded, never thrown away, and a
unit test upgrades a Phase 1 database through every migration - existing accounts are
marked verified so the upgrade cannot lock anyone out.

### Importing the old JSON files

On start-up, if the `users` / `items` tables are empty and `users.json` / `items.json`
exist in `CC_DATA_DIR`, the rows are imported once (ids and password hashes preserved).
The old files are left untouched, so you can compare before deleting them.

## Tests

```
make test              # 224 unit checks: DSA, database, repos, auth, sessions, import
sh scripts/smoke.sh    # 79 end-to-end checks: real HTTP, permissions, restart, verification, reset, lockout, claims, pagination
make asan              # build with AddressSanitizer + UBSan (slower, finds memory bugs)
```

The smoke test uses port 18080, so it never fights with a server you have open on 8080.

## Project structure

Headers declare, `.cpp` files define. One exception is explained below.

```
backend/
  main.cpp              HTTP routes, validation, configuration, start-up wiring

  db.h / db.cpp         RAII wrapper over the SQLite C API
  schema.h / schema.cpp table definitions + versioned migrations

  user_repo.h / .cpp    every SQL statement for users (+ the User record)
  item_repo.h / .cpp    every SQL statement for items

  store.h / store.cpp   notices in memory: HashTable + Trie + Heap over the database
  auth.h / auth.cpp     Argon2id hashing, sessions, lockout, roles
  session_repo.h / .cpp login sessions in SQLite (only token hashes are stored)
  secrets.h / .cpp      random tokens + SHA-256 hashing
  password.h / .cpp     Argon2id password hashing
  mailer.h / .cpp       Mailer interface + console and Brevo implementations
  email_token_repo.h/.cpp  single-use verify/reset tokens
  email_service.h / .cpp   verification + password-reset flows
  rate_limiter.h / .cpp    fixed-window counters in SQLite (login / sign-up / reset limits)
  claim_repo.h / .cpp      lost & found claims: submit, list, approve / reject (transaction)
  legacy_json.h / .cpp  one-time import of Part 1's JSON files

  hashtable.h / .cpp    chaining hash table (id -> notice) that rehashes as it fills
  trie.h / trie.cpp     prefix tree over title words, O(length) prefix search
  inverted_index.h/.cpp word -> notice ids, for exact-word keyword search
  text.h / text.cpp     the one tokeniser both indexes and the query use
  heap.h                template binary heap with a custom comparator
  item.h                the Item record (a plain struct)

  httplib.h             third-party: HTTP server/client (downloaded, not committed)
  json.hpp              third-party: JSON (downloaded, not committed)
frontend/               index.html, style.css, app.js
Caddyfile               HTTPS reverse-proxy config (Phase 5)
tests/                  unit tests (no framework)
scripts/                fetch_deps.sh, smoke.sh
docs/                   change notes and study list
build/                  object files produced by make (gitignored)
```

`heap.h` has no `.cpp` on purpose: a class template is not compiled until it is
used, so the compiler needs the full definition at the point of use. Templates
stay in headers; everything that is not a template is declared in a header and
defined in exactly one `.cpp`.

## Who can do what

| Action | Not logged in | Student | Admin |
|---|---|---|---|
| See notices | no | yes | yes |
| Post a notice | no | yes, after verifying their email (posted under their own name) | yes |
| Mark as sold / returned / found / claimed | no | only their own notices | any notice |
| Delete a notice | no | only their own notices | any notice |
| Become admin | no | no (only via server settings) | — |
| Log out everywhere | no | own account | own account |
| Verify email / reset password | no | own account, via an emailed single-use link | own account |
| Claim a lost/found notice | no | yes, with an answer and a contact method | yes |
| Approve or reject claims | no | only on their own notices | any notice |

These rules are checked by the server on every request. The frontend only hides buttons.

## Security measures

- Passwords hashed with Argon2id (libsodium); never stored or logged in plain text.
  `users` table rows are protected by the OS file permissions of `campus.db`.
- SQL injection is impossible in practice: every query is a prepared statement with
  bound parameters. No SQL string is ever built from user input.
- Sessions: random 256-bit token in an `HttpOnly; SameSite=Strict` cookie (add `Secure`
  with `CC_COOKIE_SECURE=1`), 24-hour expiry. The database stores only the token's
  SHA-256 hash, sessions survive a restart, and every session is revocable (log out all).
  At most 10 sessions are kept per account, newest first.
- Email verification: a single-use link (24 h) whose token is stored only as a SHA-256
  hash. Re-sending is limited to once a minute. Unverified accounts can browse but not post.
- Password reset: a single-use link (1 h); weak passwords are refused; a successful reset
  revokes every session. `/api/forgot` answers the same for unknown addresses, so it cannot
  be used to discover which emails have accounts.
- Rate limiting is enforced server-side and stored in SQLite: 5 failed logins per account
  per 15 minutes, 20 per network per 15 minutes, 10 sign-ups per network per hour, and
  bounded reset/resend requests. A successful login forgives the account but not the
  network, so an attacker cannot reset the IP counter by logging into their own account.
- HTTPS is terminated by a proxy (Caddy); the session cookie is `Secure` when
  `CC_COOKIE_SECURE=1`, and `CC_HSTS=1` sends HSTS. The app itself never handles
  certificates.
- Lost & found privacy: the owner's private detail and contact are never part of the
  public notice JSON. Claims are visible only to the notice owner and admins, and the
  owner's contact is revealed to a claimant only after the owner approves that claim.
- Login: same error for wrong email or wrong password; one password hash is verified even
  for unknown emails so response time does not reveal which accounts exist; 5 failed
  attempts per email locks that email for 15 minutes.
- Cross-site requests: no CORS headers, `SameSite=Strict` cookies, and POST/DELETE with a
  foreign `Origin` header are rejected.
- Input limits: 16 KB request bodies, length limits on every field, price 0 to 1,000,000,
  `CHECK` constraints in the database as a second line of defence.
- Headers: Content-Security-Policy, `X-Frame-Options: DENY`, `nosniff`.
- The frontend inserts text with `textContent`, never `innerHTML`.
- Unexpected exceptions are logged with their real message on the server, but the browser
  only ever receives a generic `500 internal server error`.

### Known limitations (planned for Part 2)

- No HTTPS by itself. Put Caddy or nginx in front for real use, otherwise passwords cross
  the network unencrypted.
- No screen to list or revoke individual active devices yet (only "log out everywhere").
- Email delivery depends on configuration: the default console mailer only prints links
  (perfect for a local demo, useless in production). Real delivery needs
  `CC_MAIL_MODE=brevo`, an API key and a verified sender address; SPF/DKIM setup is out of
  scope. Nothing checks that the domain in `CC_EMAIL_DOMAIN` belongs to the college until
  the verification email is actually delivered.
- The per-email lockout can still be abused on purpose: an attacker can lock a known
  address by failing five times. Per-IP limits bound how many addresses one network can
  attack, but they are not a complete fix (CAPTCHA or proof-of-work would be). On a
  campus network many students share one public IP, so the per-IP limits should be tuned
  for that - or `CC_TRUST_PROXY` used behind a proxy that reports real addresses.
- Claim approval is manual: the owner decides, and while an admin can also decide through
  the API there is no admin dashboard for disputes. There is no appeals flow, and the
  contact details are user-typed text (nothing verifies a phone number).
- Claim limits: a person can send at most one claim per notice (enforced by a UNIQUE
  constraint), and claims are only possible on open lost/found notices.
- HTTPS needs a proxy; the C++ binary itself does not speak TLS. That keeps certificate
  handling out of the application, but it means the deployment must include Caddy/nginx.

## DSA used

| Structure | File | Used for |
|---|---|---|
| HashTable (chaining, **rehashing**) | `hashtable.h` | notice lookup by id, O(1) average; grows at load factor 0.75 into a prime-sized table |
| Trie | `trie.h` | prefix search on title words ("cal" -> calculator) |
| **InvertedIndex** (`word -> ids`) | `inverted_index.h` | exact-word search ("wallet"), unioned with the trie's prefix matches |
| Heap (custom comparator) | `heap.h` | **bounded top-K** for pagination: O(n log K) instead of sorting the whole board |
| Hash map (`std::unordered_map`) | `auth.cpp` | recent failed logins. Sessions are SQLite rows now. |

Why both a trie and an inverted index? They answer different questions. The trie answers
"starts with" in O(length of the query), which is what an autocomplete needs; the inverted
index answers "contains this word" in O(1) average. `Store::query` unions the two per query
word and ANDs across words, so "cal" and "calculator" both work.

Pagination is a top-K problem: to return items `[offset, offset+limit)`, only the first
`offset + limit` items in the requested order matter. A heap ordered worst-first keeps at
most that many, discarding the worst when it overflows - so a page costs O(n log K) time
and O(K) memory rather than sorting all n notices. (Deep pages make K large, so a very
large offset degrades towards the full sort; keyset pagination is the next step if it ever
matters.)

Where the database fits: SQLite stores rows and answers ordered range queries with a
B+ tree index. The in-memory structures answer point, prefix and keyword lookups. On
start-up the indexes are rebuilt from the database (O(n), once); every write goes to the
database first and then to memory. That ordering is why a crash cannot leave memory and
disk disagreeing about anything that was already acknowledged to the user.

## Roadmap (Part 2)

Done: sessions in SQLite (Phase 2), email verification and password reset (Phase 3),
database-backed rate limiting per account and per network (Phase 4), HTTPS deployment with
Caddy (Phase 5), claim verification for lost & found (Phase 6), DSA upgrades (Phase 7).

The original Part 2 plan is complete. Optional extras (not required):

1. Admin dashboard for disputes and stats.
2. In-app notifications / an SSE live board.
3. A background queue for sending email.
4. Item images, keyset pagination.


---

## Live demo

**Try CampusConnect:** https://furnished-cds-interpretation-visiting.trycloudflare.com

This demo is hosted on AWS EC2 and made public through a Cloudflare Quick Tunnel. Quick Tunnel URLs are temporary and can change when the tunnel is recreated. If the link stops working, the current URL must be read from the running tunnel and this README updated.

---

# AWS EC2 deployment and live email delivery

CampusConnect has been deployed to an **AWS EC2 virtual machine** so it can be accessed over the internet instead of only running on a developer's laptop. This section records the deployment architecture and the production issues fixed while bringing the application online.

## Deployment overview

The demo deployment uses:

- **AWS EC2** running Ubuntu Server 24.04 LTS.
- A small `t3.micro` instance with an 8 GiB EBS root volume.
- The C++17 application built on the instance and listening on port 8080.
- **systemd** to start CampusConnect automatically at boot and restart it if the process exits.
- **Cloudflare Quick Tunnel** to provide a public HTTPS URL without opening the application port to the internet.
- **SQLite** for persistent application data on the EC2 EBS volume.
- **Brevo** for real verification and password-reset email delivery.

The public URL generated by a Cloudflare Quick Tunnel is temporary and may change when the tunnel is recreated. The configured `CC_BASE_URL` must always match the current public HTTPS URL so email links point to the correct site.

## AWS networking and access

The EC2 security group should allow SSH (TCP 22) only from the administrator's current public IP address. Port 8080 should not be exposed publicly: the app is accessed locally by Cloudflare Tunnel. Do not open SSH to `0.0.0.0/0`.

The EC2 private key belongs on the administrator's computer only. Never commit it to this repository.

## Running the application on EC2

The deployment uses a systemd unit at `/etc/systemd/system/campusconnect.service`:

```ini
[Unit]
Description=CampusConnect Web Application
After=network.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/home/ubuntu/campusconnect/backend
EnvironmentFile=/etc/campusconnect.env
ExecStart=/home/ubuntu/campusconnect/backend/campusconnect
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

It is enabled with `systemctl enable --now campusconnect`. The service runs independently of the SSH session and starts again after an instance reboot. Useful commands:

```bash
sudo systemctl status campusconnect --no-pager
sudo systemctl restart campusconnect
sudo journalctl -u campusconnect -n 100 --no-pager
```

Cloudflare Tunnel is also managed by systemd on the EC2 host. Its origin is `http://localhost:8080`; no inbound application port is required. A Quick Tunnel URL can change, so update `CC_BASE_URL` in the server's environment file and restart the app whenever the URL changes.

## Brevo email configuration

The application supports two mail modes:

- `CC_MAIL_MODE=console`: prints email links to the server console for local development.
- `CC_MAIL_MODE=brevo`: sends real email through the Brevo API.

For the EC2 deployment, Brevo was configured with server-side environment variables in `/etc/campusconnect.env`:

```ini
CC_MAIL_MODE=brevo
CC_BREVO_API_KEY=YOUR_BREVO_API_KEY
CC_MAIL_FROM=YOUR_VERIFIED_SENDER_ADDRESS
CC_MAIL_FROM_NAME=CampusConnect
CC_BASE_URL=https://YOUR-CURRENT-PUBLIC-HTTPS-URL
```

**Do not put real values in this repository.** The API key and sender address belong only in the private server environment file. The environment file should be owned by root and have restrictive permissions (for example, mode 600).

Brevo requires the sender address configured in `CC_MAIL_FROM` to be verified in the Brevo account (or to belong to an authenticated sending domain). The development default `no-reply@campusconnect.local` is not a valid real-world sender for Brevo. The live delivery issue was fixed by setting `CC_MAIL_FROM` to the verified sender on EC2, then restarting the CampusConnect service. The verification email was subsequently received successfully.

## Email verification failure cleanup

A failed email send must not leave a fresh verification token behind. Otherwise, a user may be blocked by the resend cooldown even though no message reached their inbox.

The email service now deletes the newly created verification token when the mailer reports a delivery failure. `EmailTokenRepo::deleteToken` performs the token deletion by its hash. This cleanup is limited to the token created for the failed send; it does not expose or log the raw token.

## SQLite persistence and backups

The SQLite database is stored under the configured data directory (by default, `backend/data/campus.db`). On EC2 this directory is on the instance's EBS volume, so data survives application and instance restarts. It does not replace backups: deleting the instance or its volume can still delete the database. Back up the database to secure storage outside the instance and protect those backups because they contain account information.

## Updating the EC2 deployment

After changes have been tested and pushed to GitHub:

```bash
cd ~/campusconnect
git pull
make
make test
sudo systemctl restart campusconnect
sudo systemctl status campusconnect --no-pager
```

Keep `/etc/campusconnect.env`, private keys, database files, backups, and generated runtime data out of Git. Never paste API keys or server credentials into source files, commits, issues, or public documentation.

For the full first-time setup and maintenance instructions, see the [AWS EC2 deployment guide](docs/AWS-DEPLOYMENT.md).
