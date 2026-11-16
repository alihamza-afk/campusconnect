#!/bin/sh
# ---------------------------------------------------------------------------
# End-to-end smoke test: starts the real server, talks to it with curl, kills it,
# starts it again, and checks that everything survived the restart.
#
# Run with:  sh scripts/smoke.sh
# Requires:  the backend built first (make)
# ---------------------------------------------------------------------------
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/backend/campusconnect"
DATA=$(mktemp -d)
PORT=18080   # test port, so a real server on 8080 is never disturbed
BASE="http://127.0.0.1:$PORT"
JAR1="$DATA/jar1"
JAR2="$DATA/jar2"
JAR1B="$DATA/jar1b"
PASS=0
FAIL=0
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; fi
    rm -rf "$DATA"
}
trap cleanup EXIT

check() {  # label expected actual
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $1 (expected '$2', got '$3')"
    fi
}

start_server() {
    # exec replaces the subshell with the server, so $! is the server's real pid
    # (otherwise we would kill the wrapper and leave the server running).
    (
        cd "$ROOT/backend"
        export CC_DATA_DIR="$DATA" CC_FRONTEND_DIR="$ROOT/frontend" CC_PORT="$PORT" CC_HSTS=1
        exec "$BIN" >>"$DATA/server.log" 2>&1
    ) &
    SERVER_PID=$!
    echo "$SERVER_PID" >"$DATA/pid"
    i=0
    while [ $i -lt 100 ]; do
        if curl -s -o /dev/null "$BASE/api/me"; then return 0; fi
        i=$((i + 1))
        sleep 0.1
    done
    echo "server did not start; log:"; cat "$DATA/server.log"; exit 1
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    sleep 0.3
}

# ---------------------------------------------------------------------------
# Part 1 leftovers: these must be imported on first start.
# ---------------------------------------------------------------------------
cat >"$DATA/users.json" <<'JSON'
{"nextId": 2, "users": [{"id": 1, "email": "legacy@geu.ac.in", "name": "Legacy User",
  "passHash": "not-a-real-hash", "role": "user"}]}
JSON
cat >"$DATA/items.json" <<'JSON'
{"nextId": 2, "items": [{"id": 1, "type": "sell", "title": "Old calculator",
  "description": "works fine", "category": "Electronics", "location": "Hostel C",
  "owner": "Legacy User", "ownerId": 1, "price": 120, "createdAt": 1700000000,
  "status": "open"}]}
JSON

start_server
check "server created campus.db" "yes" "$( [ -f "$DATA/campus.db" ] && echo yes || echo no )"
check "imported legacy rows are announced" "yes" \
      "$(grep -q 'Imported' "$DATA/server.log" && echo yes || echo no)"

# ---------------------------------------------------------------------------
# Accounts and permissions
# ---------------------------------------------------------------------------
code=$(curl -s -o "$DATA/reg" -w '%{http_code}' -c "$JAR1" -X POST "$BASE/api/register" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u1@geu.ac.in","name":"User One","password":"password123"}')
check "register user 1" 201 "$code"
UID1=$(python3 -c "import json;print(json.load(open('$DATA/reg'))['id'])")

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/register" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u1@geu.ac.in","name":"Copy","password":"password123"}')
check "duplicate email rejected" 400 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/register" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u3@geu.ac.in","name":"Weak","password":"short"}')
check "short password rejected" 400 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/items")
check "items need login" 401 "$code"

# --- Phase 3: an unverified account can browse but cannot post ---
check "new account starts unverified" "False" \
      "$(python3 -c "import json;print(json.load(open('$DATA/reg'))['emailVerified'])")"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" "$BASE/api/items")
check "unverified users can still browse" 200 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/items" \
       -H 'Content-Type: application/json' \
       -d '{"type":"sell","title":"Blocked","description":"","category":"Other","location":"","price":10}')
check "unverified users cannot post" 403 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/verify/resend")
check "resend respects the cooldown" 429 "$code"

VERIFY_TOKEN=$(grep -o 'verify=[a-f0-9]*' "$DATA/server.log" | head -1 | cut -d= -f2)
check "verification link was emailed" "yes" "$( [ -n "$VERIFY_TOKEN" ] && echo yes || echo no )"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/verify" \
       -H 'Content-Type: application/json' -d "{\"token\":\"$VERIFY_TOKEN\"}")
check "email verification works" 200 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/verify" \
       -H 'Content-Type: application/json' -d "{\"token\":\"$VERIFY_TOKEN\"}")
check "verification link is single-use" 400 "$code"

curl -s -b "$JAR1" "$BASE/api/me" >"$DATA/me"
check "account is verified now" "True" \
      "$(python3 -c "import json;print(json.load(open('$DATA/me'))['emailVerified'])")"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/verify/resend")
check "resend after verification is refused" 400 "$code"

code=$(curl -s -o "$DATA/posted" -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/items" \
       -H 'Content-Type: application/json' \
       -d '{"type":"sell","title":"Drafting board","description":"A2 size","category":"Stationery","location":"Library","price":250}')
check "verified user can post" 201 "$code"
ITEM2=$(python3 -c "import json;print(json.load(open('$DATA/posted'))['id'])")

curl -s -b "$JAR1" "$BASE/api/items" >"$DATA/list"
COUNT=$(python3 -c "import json;print(len(json.load(open('$DATA/list'))['items']))")
check "legacy notice + new notice on the board" 2 "$COUNT"

OWNER=$(python3 -c "import json;print(json.load(open('$DATA/list'))['items'][0]['owner'])")
check "owner name comes from the users table (JOIN)" "User One" "$OWNER"

curl -s -b "$JAR1" "$BASE/api/items?q=calc" >"$DATA/search"
check "prefix search finds the legacy notice" 1 \
      "$(python3 -c "import json;print(json.load(open('$DATA/search'))['total'])")"

curl -s -b "$JAR1" "$BASE/api/items?q=wallet" >"$DATA/search2"
check "whole-word search works too" 0 \
      "$(python3 -c "import json;print(json.load(open('$DATA/search2'))['total'])")"

curl -s -b "$JAR1" "$BASE/api/items?sort=cheap" >"$DATA/sorted"
FIRST=$(python3 -c "import json;print(json.load(open('$DATA/sorted'))['items'][0]['price'])")
check "cheapest-first sorting" 120.0 "$FIRST"

# Phase 7: pagination / top-K
curl -s -b "$JAR1" "$BASE/api/items?limit=1&offset=0" >"$DATA/page1"
check "pagination returns one item" 1 \
      "$(python3 -c "import json;print(len(json.load(open('$DATA/page1'))['items']))")"
check "pagination reports the matching total" 2 \
      "$(python3 -c "import json;print(json.load(open('$DATA/page1'))['total'])")"
PAGE1_ID=$(python3 -c "import json;print(json.load(open('$DATA/page1'))['items'][0]['id'])")
curl -s -b "$JAR1" "$BASE/api/items?limit=1&offset=1" >"$DATA/page2"
PAGE2_ID=$(python3 -c "import json;print(json.load(open('$DATA/page2'))['items'][0]['id'])")
check "offset returns a different item" "yes" "$( [ "$PAGE1_ID" != "$PAGE2_ID" ] && echo yes || echo no )"
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" "$BASE/api/items?limit=0")
check "invalid limit rejected" 400 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR2" -X POST "$BASE/api/register" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u2@geu.ac.in","name":"User Two","password":"password123"}')
check "register user 2" 201 "$code"

U2_TOKEN=$(grep -o 'verify=[a-f0-9]*' "$DATA/server.log" | tail -1 | cut -d= -f2)
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/verify" \
       -H 'Content-Type: application/json' -d "{\"token\":\"$U2_TOKEN\"}")
check "second user verifies email" 200 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" -X POST "$BASE/api/items/$ITEM2/close")
check "someone else cannot close your notice" 403 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" -X DELETE "$BASE/api/items/$ITEM2")
check "someone else cannot delete your notice" 403 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" -X DELETE "$BASE/api/items/1")
check "you cannot delete a legacy notice you do not own" 403 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/items/$ITEM2/close")
check "owner can close own notice" 200 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/items/99999/close")
check "closing a missing notice gives 404" 404 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X DELETE "$BASE/api/items/$ITEM2")
check "owner can delete own notice" 200 "$code"


# ---------------------------------------------------------------------------
# Phase 6: lost & found claim verification
# ---------------------------------------------------------------------------
code=$(curl -s -o "$DATA/lost" -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/items" \
       -H 'Content-Type: application/json' \
       -d '{"type":"lost","title":"Blue water bottle","description":"Lost near the library","category":"Other","location":"Library","price":0,"proof":"cat sticker on the base","contact":"Hostel C room 12"}')
check "post a lost notice with a private detail" 201 "$code"
LOST_ID=$(python3 -c "import json;print(json.load(open('$DATA/lost'))['id'])")

curl -s -b "$JAR2" "$BASE/api/items" >"$DATA/board2"
check "private detail and contact are never exposed to other users" "no" \
      "$(python3 -c "import json;print('yes' if any('proof' in i or 'contact' in i for i in json.load(open('$DATA/board2'))['items']) else 'no')")"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" -X POST "$BASE/api/items/1/claims" \
       -H 'Content-Type: application/json' -d '{"answer":"x","contact":"y"}')
check "claims only work on lost/found notices" 400 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/items/$LOST_ID/claims" \
       -H 'Content-Type: application/json' -d '{"answer":"x","contact":"y"}')
check "cannot claim your own notice" 400 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" -X POST "$BASE/api/items/$LOST_ID/claims" \
       -H 'Content-Type: application/json' -d '{"answer":"","contact":""}')
check "a claim needs an answer and a contact" 400 "$code"

code=$(curl -s -o "$DATA/claim" -w '%{http_code}' -b "$JAR2" -X POST "$BASE/api/items/$LOST_ID/claims" \
       -H 'Content-Type: application/json' \
       -d '{"answer":"It has a cat sticker on the base","message":"Picked it up in the library","contact":"u2 phone"}')
check "another user can claim a lost notice" 201 "$code"
CLAIM_ID=$(python3 -c "import json;print(json.load(open('$DATA/claim'))['id'])")

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" -X POST "$BASE/api/items/$LOST_ID/claims" \
       -H 'Content-Type: application/json' -d '{"answer":"again","contact":"x"}')
check "duplicate claim rejected" 400 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" "$BASE/api/items/$LOST_ID/claims")
check "claimant cannot see the notice's claims" 403 "$code"

code=$(curl -s -o "$DATA/claims" -w '%{http_code}' -b "$JAR1" "$BASE/api/items/$LOST_ID/claims")
check "owner can list claims" 200 "$code"
check "the claim includes the answer and the contact" "yes" \
      "$(python3 -c "import json;c=json.load(open('$DATA/claims'))[0];print('yes' if c['answer'] and c['contact'] else 'no')")"

curl -s -b "$JAR2" "$BASE/api/claims/mine" >"$DATA/mine"
check "claimant sees the claim as pending" "pending" \
      "$(python3 -c "import json;print(json.load(open('$DATA/mine'))[0]['status'])")"
check "owner contact is hidden before approval" "" \
      "$(python3 -c "import json;print(json.load(open('$DATA/mine'))[0]['ownerContact'])")"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" -X POST "$BASE/api/items/$LOST_ID/claims/$CLAIM_ID/decide" \
       -H 'Content-Type: application/json' -d '{"approve":true}')
check "only the owner or an admin can approve" 403 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/items/$LOST_ID/claims/$CLAIM_ID/decide" \
       -H 'Content-Type: application/json' -d '{"approve":true}')
check "owner approves the claim" 200 "$code"

curl -s -b "$JAR1" "$BASE/api/items" >"$DATA/afterclaim"
check "approved notice is no longer on the board" "no" \
      "$(python3 -c "import json;print('yes' if any(i['id'] == $LOST_ID for i in json.load(open('$DATA/afterclaim'))['items']) else 'no')")"

curl -s -b "$JAR2" "$BASE/api/claims/mine" >"$DATA/mine2"
check "claimant sees it approved with the owner's contact" "yes" \
      "$(python3 -c "import json;c=json.load(open('$DATA/mine2'))[0];print('yes' if c['status'] == 'approved' and c['ownerContact'] else 'no')")"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/items/$LOST_ID/claims/$CLAIM_ID/decide" \
       -H 'Content-Type: application/json' -d '{"approve":true}')
check "a claim cannot be approved twice" 400 "$code"

# ---------------------------------------------------------------------------
# Headers and static frontend
# ---------------------------------------------------------------------------
HEADERS=$(curl -s -D - -o /dev/null -b "$JAR1" "$BASE/api/items")
check "CSP header present" "yes" "$(echo "$HEADERS" | grep -qi 'content-security-policy' && echo yes || echo no)"
check "HSTS header present when enabled" "yes" "$(echo "$HEADERS" | grep -qi 'strict-transport-security' && echo yes || echo no)"
check "frontend index is served" 200 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/")"

# ---------------------------------------------------------------------------
# Restart: accounts, notices AND sessions must survive.
# ---------------------------------------------------------------------------
stop_server
start_server

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" "$BASE/api/me")
check "session survived the restart" 200 "$code"

check "sessions table stores 64-character token hashes" 64 \
      "$(sqlite3 "$DATA/campus.db" 'SELECT length(token_hash) FROM sessions LIMIT 1;')"

# Same account, second device.
code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR1B" -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u1@geu.ac.in","password":"password123"}')
check "second device logged in" 200 "$code"

code=$(curl -s -o "$DATA/logoutall" -w '%{http_code}' -b "$JAR1" -X POST "$BASE/api/logout-all")
check "log out everywhere works" 200 "$code"
check "both sessions ended" 2 \
      "$(python3 -c "import json;print(json.load(open('$DATA/logoutall'))['ended'])")"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" "$BASE/api/me")
check "first device logged out by logout-all" 401 "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1B" "$BASE/api/me")
check "second device logged out by logout-all" 401 "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" "$BASE/api/me")
check "logout-all does not touch other accounts" 200 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR1" -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u1@geu.ac.in","password":"password123"}')
check "login works again after logout-all" 200 "$code"

curl -s -b "$JAR1" "$BASE/api/items" >"$DATA/after"
COUNT=$(python3 -c "import json;print(len(json.load(open('$DATA/after'))['items']))")
check "open notices survived the restart" 1 "$COUNT"
check "deleted notice stayed deleted" "yes" \
      "$(python3 -c "import json;print('yes' if all(i['title'] != 'Drafting board' for i in json.load(open('$DATA/after'))['items']) else 'no')")"
check "legacy JSON files were not modified" "yes" \
      "$(grep -q 'Legacy User' "$DATA/users.json" && echo yes || echo no)"


# ---------------------------------------------------------------------------
# Password reset (Phase 3)
# ---------------------------------------------------------------------------
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/forgot" \
       -H 'Content-Type: application/json' -d '{"email":"nobody@geu.ac.in"}')
check "forgot answers the same for unknown emails" 200 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/forgot" \
       -H 'Content-Type: application/json' -d '{"email":"u1@geu.ac.in"}')
check "forgot password request accepted" 200 "$code"

RESET_TOKEN=$(grep -o 'reset=[a-f0-9]*' "$DATA/server.log" | tail -1 | cut -d= -f2)
check "reset link was emailed" "yes" "$( [ -n "$RESET_TOKEN" ] && echo yes || echo no )"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/reset" \
       -H 'Content-Type: application/json' -d '{"token":"wrong-token","password":"newpassword123"}')
check "bad reset token rejected" 400 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/reset" \
       -H 'Content-Type: application/json' -d "{\"token\":\"$RESET_TOKEN\",\"password\":\"newpassword123\"}")
check "password reset works" 200 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR1" "$BASE/api/me")
check "reset revoked the old session" 401 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u1@geu.ac.in","password":"password123"}')
check "old password no longer works" 401 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR1" -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u1@geu.ac.in","password":"newpassword123"}')
check "new password works" 200 "$code"


# ---------------------------------------------------------------------------
# Phase 4: rate limiting (SQLite-backed, per account AND per network)
# ---------------------------------------------------------------------------
# Five wrong passwords lock that account.
i=1
while [ $i -le 5 ]; do
    curl -s -o /dev/null -X POST "$BASE/api/login" -H 'Content-Type: application/json' \
         -d '{"email":"u2@geu.ac.in","password":"wrong-password"}'
    i=$((i + 1))
done
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' -d '{"email":"u2@geu.ac.in","password":"password123"}')
check "5 failures lock the account" 429 "$code"

ROWS=$(sqlite3 "$DATA/campus.db" 'SELECT COUNT(*) FROM rate_limits;')
check "rate-limit counters are rows in SQLite" "yes" "$( [ "$ROWS" -gt 0 ] && echo yes || echo no )"

# A forged X-Forwarded-For must not be believed while CC_TRUST_PROXY is off.
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' -H 'X-Forwarded-For: 9.9.9.9' \
       -d '{"email":"u2@geu.ac.in","password":"password123"}')
check "spoofed X-Forwarded-For cannot bypass the lockout" 429 "$code"

# Enough failures from one network lock that network, even with a correct password.
i=1
while [ $i -le 25 ]; do
    curl -s -o /dev/null -X POST "$BASE/api/login" -H 'Content-Type: application/json' \
         -d "{\"email\":\"nobody$i@geu.ac.in\",\"password\":\"x\"}"
    i=$((i + 1))
done
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' -d '{"email":"u1@geu.ac.in","password":"newpassword123"}')
check "per-IP limit blocks a spray from one network" 429 "$code"

# The counters are database rows, so a restart does not clear them.
stop_server
start_server
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' -d '{"email":"u1@geu.ac.in","password":"newpassword123"}')
check "per-IP limit survives a restart" 429 "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' -d '{"email":"u2@geu.ac.in","password":"password123"}')
check "account lockout survives a restart" 429 "$code"

# ---------------------------------------------------------------------------
# Phase 8: mail queue, images, notifications, admin dashboard, user banning
# ---------------------------------------------------------------------------
stop_server   # <--- FIX: Stop the Phase 4 server so the port is free!

# After the rate-limit tests, the per-network lock is in SQLite (15-min window).
# Start a fresh data directory so rate limits from the spray test cannot interfere.
DATA8=$(mktemp -d)
(
    cd "$ROOT/backend"
    export CC_DATA_DIR="$DATA8" CC_FRONTEND_DIR="$ROOT/frontend" CC_PORT="$PORT" CC_HSTS=1
    export CC_ADMIN_EMAIL="admin@geu.ac.in" CC_ADMIN_NAME="Admin" CC_ADMIN_PASSWORD="admin_pass_123"
    exec "$BIN" >>"$DATA8/server.log" 2>&1
) &
SERVER_PID=$!
_i=0
while [ $_i -lt 100 ]; do
    if curl -s -o /dev/null "$BASE/api/me"; then break; fi
    _i=$((_i + 1)); sleep 0.1
done

JAR3="$DATA8/jar3"; JAR4="$DATA8/jar4"; ADMIN_JAR="$DATA8/admin_jar"

# Admin login.
code=$(curl -s -o /dev/null -w '%{http_code}' -c "$ADMIN_JAR" -X POST "$BASE/api/login" \
       -H 'Content-Type: application/json' \
       -d '{"email":"admin@geu.ac.in","password":"admin_pass_123"}')
check "admin logs in for Phase 8 tests" 200 "$code"

# Register u3 (fresh database, no rate limits).
code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR3" -X POST "$BASE/api/register" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u3@geu.ac.in","name":"User Three","password":"password123"}')
check "register u3 for Phase 8 tests" 201 "$code"
U3_VERIFY=$(grep -o 'verify=[a-f0-9]*' "$DATA8/server.log" | tail -1 | cut -d= -f2)
curl -s -o /dev/null -X POST "$BASE/api/verify" \
     -H 'Content-Type: application/json' -d "{\"token\":\"$U3_VERIFY\"}"

# Register u4 for banning tests.
code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR4" -X POST "$BASE/api/register" \
       -H 'Content-Type: application/json' \
       -d '{"email":"u4@geu.ac.in","name":"User Four","password":"password123"}')
check "register u4 for ban test" 201 "$code"

# Admin stats.
code=$(curl -s -o "$DATA8/stats" -w '%{http_code}' -b "$ADMIN_JAR" "$BASE/api/admin/stats")
check "admin stats returns 200" 200 "$code"
check "admin stats has users field" "yes" \
      "$(python3 -c "import json;d=json.load(open('$DATA8/stats'));print('yes' if 'users' in d else 'no')")"
check "admin stats has openItems field" "yes" \
      "$(python3 -c "import json;d=json.load(open('$DATA8/stats'));print('yes' if 'openItems' in d else 'no')")"

# Non-admin cannot access admin stats.
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR3" "$BASE/api/admin/stats")
check "non-admin cannot access admin stats" 403 "$code"

# Admin user list.
code=$(curl -s -o "$DATA8/admin_users" -w '%{http_code}' -b "$ADMIN_JAR" "$BASE/api/admin/users")
check "admin user list returns 200" 200 "$code"
# cations: u3 posts, admin claims, u3 gets notification.
code=$(curl -s -o "$DATA8/notif_post" -w '%{http_code}' -b "$JAR3" -X POST "$BASE/api/items" \
       -H 'Content-Type: application/json' \
       -d '{"type":"lost","title":"Notification bottle","description":"notif","category":"Other","location":"Library","price":0,"proof":"secret","contact":"u3 contact"}')
check "u3 posts notice for notification test" 201 "$code"
NOTIF_ITEM_ID=$(python3 -c "import json;print(json.load(open('$DATA8/notif_post'))['id'])")

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$ADMIN_JAR" -X POST "$BASE/api/items/$NOTIF_ITEM_ID/claims" \
       -H 'Content-Type: application/json' -d '{"answer":"secret","contact":"admin contact"}')
check "admin claims u3 notice" 201 "$code"

curl -s -b "$JAR3" "$BASE/api/notifications" >"$DATA8/notifs"
check "u3 receives claim notification" "yes" \
      "$(python3 -c "import json;n=json.load(open('$DATA8/notifs'));print('yes' if len(n)>=1 else 'no')")"
check "notification kind is claim" "claim" \
      "$(python3 -c "import json;n=json.load(open('$DATA8/notifs'));print(n[0]['kind'])")"
check "notification is unread" "False" \
      "$(python3 -c "import json;n=json.load(open('$DATA8/notifs'));print(n[0]['read'])")"

curl -s -b "$JAR3" "$BASE/api/me" >"$DATA8/me_notif"
check "me includes unreadNotifications" "yes" \
      "$(python3 -c "import json;d=json.load(open('$DATA8/me_notif'));print('yes' if 'unreadNotifications' in d else 'no')")"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR3" -X POST "$BASE/api/notifications/read")
check "mark notifications read returns 200" 200 "$code"
curl -s -b "$JAR3" "$BASE/api/notifications" >"$DATA8/notifs2"
check "notifications are now read" "True" \
      "$(python3 -c "import json;n=json.load(open('$DATA8/notifs2'));print(n[0]['read'])")"

# Image upload.
printf '\211PNG\r\n\032\ndata' > "$DATA8/test.png"
code=$(curl -s -o "$DATA8/img_upload" -w '%{http_code}' -b "$JAR3" -X POST "$BASE/api/items/$NOTIF_ITEM_ID/image" \
       -H 'Content-Type: image/png' --data-binary "@$DATA8/test.png")
check "image upload returns 200" 200 "$code"
check "image upload returns mime" "image/png" \
      "$(python3 -c "import json;print(json.load(open('$DATA8/img_upload'))['mime'])")"

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/items/$NOTIF_ITEM_ID/image")
check "image GET returns 200" 200 "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR3" -X POST "$BASE/api/items/$NOTIF_ITEM_ID/image" \
       -H 'Content-Type: text/plain' --data-binary "hello world")
check "non-image upload rejected" 400 "$code"

curl -s -b "$JAR3" "$BASE/api/items" >"$DATA8/board_img"
check "board shows hasImage for uploaded item" "yes" \
      "$(python3 -c "import json;items=json.load(open('$DATA8/board_img'))['items'];print('yes' if any(i.get('hasImage') for i in items) else 'no')")"

stop_server
rm -rf "$DATA8"


echo ""
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || { echo "server log:"; cat "$DATA/server.log"; exit 1; }
