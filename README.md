# CampusConnect (Part 1 of 2, about 40% of the project)

A campus noticeboard for selling, lending, and lost & found items.

Frontend: HTML, CSS, JavaScript (frontend/)
Backend: C++17 with DSA (backend/)

## Run
1. Install libsodium (password hashing): `brew install libsodium` on macOS, `sudo apt install libsodium-dev` on Debian/Ubuntu.
2. Download into backend/:
   - `httplib.h` from https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h
   - `json.hpp` from https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
3. Build:
   ```
   cd backend
   g++ -std=c++17 main.cpp -o campusconnect -pthread $(pkg-config --cflags --libs libsodium)
   ```
4. Start it with an admin account (only needed the first time; the admin is saved):
   ```
   CC_ADMIN_EMAIL=admin@geu.ac.in CC_ADMIN_NAME="Lost & Found Desk" CC_ADMIN_PASSWORD='choose-a-long-password' ./campusconnect
   ```
5. Open http://localhost:8080, create a student account, and log in.

Always open the site through the server (http://localhost:8080), not by double-clicking index.html.
There is no sample or demo data: every notice is saved in `backend/data/items.json` and stays
until it is deleted, across page refreshes and server restarts. To start over with an empty board,
stop the server and delete the `backend/data` folder (this also deletes all accounts).

With Docker: `docker build -t campusconnect . && docker run -p 8080:8080 -v campusdata:/app/backend/data -e CC_ADMIN_EMAIL=... -e CC_ADMIN_PASSWORD=... campusconnect`

### Settings (environment variables)
| Variable | Meaning | Default |
|---|---|---|
| `CC_ADMIN_EMAIL`, `CC_ADMIN_NAME`, `CC_ADMIN_PASSWORD` | Creates the first admin if no admin exists. Ignored afterwards. | none |
| `CC_EMAIL_DOMAIN` | Only allow sign-up with this email domain, e.g. `geu.ac.in` | any domain |
| `CC_DATA_DIR` | Where `items.json` and `users.json` are saved | `data` |
| `CC_COOKIE_SECURE` | Set to `1` when served over HTTPS | off |
| `CC_FRONTEND_DIR` | Folder served as the website | `../frontend` |

## Who can do what
| Action | Not logged in | Student | Admin |
|---|---|---|---|
| See notices | no | yes | yes |
| Post a notice | no | yes (posted under their own name) | yes |
| Mark as sold / returned / found / claimed | no | only their own notices | any notice |
| Delete a notice | no | only their own notices | any notice |
| Become admin | no | no (only via server settings) | — |

These rules are checked by the server on every request. The frontend only hides buttons.

## Security measures
- Passwords hashed with Argon2id (libsodium); never stored or logged in plain text. `users.json` is readable only by the server's OS user.
- Sessions: random 256-bit token in an `HttpOnly; SameSite=Strict` cookie (add `Secure` with `CC_COOKIE_SECURE=1`). Expire after 24 hours.
- Login: same error for wrong email or wrong password; 5 failed attempts per email locks that email for 15 minutes.
- Cross-site requests: no CORS headers, `SameSite=Strict` cookies, and POST/DELETE with a foreign `Origin` header are rejected.
- Input limits: 16 KB request bodies, length limits on every field, price 0 to 1,000,000.
- Headers: Content-Security-Policy, `X-Frame-Options: DENY`, `nosniff`.
- The frontend inserts text with `textContent`, never `innerHTML`.

### Known limitations (planned for Part 2)
- No HTTPS by itself. Put Caddy or nginx in front for real use, otherwise passwords cross the network unencrypted.
- No email verification, so `CC_EMAIL_DOMAIN` checks the address format only, not that the person owns it.
- The 15-minute lockout can be triggered on purpose to lock someone out (lockout is per email, not per IP).
- Sessions are kept in memory, so everyone is logged out when the server restarts.
- No claim verification yet for lost & found: the owner or admin marks it claimed, but there is no hidden detail, approval step or handover record.

## DSA used
- HashTable (chaining): item lookup by id
- Trie: prefix search on title words
- Heap (plain array that grows) with custom comparator: newest / cheapest / dearest ordering
- Hash maps (`std::unordered_map`) for sessions (token → user) and accounts (email → user)
