// ---------------------------------------------------------------------------
// tests/test_units.cpp - unit and persistence tests, no framework, no cost.
//
// Run with: make test
//
// Every check is an ordinary C++ comparison. If a check fails the program
// prints the file line and expression and exits with status 1, so it also works
// as a CI step later (GitHub Actions just runs `make test`).
// ---------------------------------------------------------------------------
#include <sodium.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "auth.h"
#include "claim_repo.h"
#include "db.h"
#include "email_service.h"
#include "email_token_repo.h"
#include "hashtable.h"
#include "heap.h"
#include "images.h"
#include "inverted_index.h"
#include "mail_queue.h"
#include "notification_repo.h"
#include "text.h"
#include "item.h"
#include "item_repo.h"
#include "legacy_json.h"
#include "mailer.h"
#include "rate_limiter.h"
#include "schema.h"
#include "session_repo.h"
#include "store.h"
#include "trie.h"
#include "user_repo.h"

static int checksRun = 0;
static int checksFailed = 0;

static void check(bool condition, const char* text, int line) {
    checksRun++;
    if (!condition) {
        checksFailed++;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, text);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

static std::filesystem::path tempDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("campusconnect_test_" + tag + "_" + std::to_string(std::time(nullptr)) + "_" +
                std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// ---------------------------------------------------------------------------
// DSA
// ---------------------------------------------------------------------------
static void testHashTable() {
    HashTable table;
    Item a; a.id = 1; a.title = "one";
    Item b; b.id = 102; b.title = "two";   // 102 % 101 == 1, so it lands in the same bucket
    table.put(a);
    table.put(b);

    CHECK(table.get(1) != nullptr);
    CHECK(table.get(2) == nullptr);
    CHECK(table.get(102) != nullptr);
    CHECK(table.get(102)->title == "two");   // collision handled by chaining
    CHECK(table.remove(1));
    CHECK(table.get(1) == nullptr);
    CHECK(table.get(102) != nullptr);        // removing one item leaves the neighbour alone

    Item updated; updated.id = 102; updated.title = "renamed";
    table.put(updated);
    CHECK(table.get(102)->title == "renamed");   // put() replaces, does not duplicate
}

static void testTrie() {
    Trie trie;
    trie.insert("Blue calculator", 1);
    trie.insert("Calculator case", 2);

    std::set<int> calc = trie.search("cal");       // prefix match
    CHECK(calc.size() == 2);
    std::set<int> both = trie.search("blue calc"); // every word must match
    CHECK(both.size() == 1 && both.count(1) == 1);
    CHECK(trie.search("zzz").empty());

    trie.erase("Blue calculator", 1);
    CHECK(trie.search("blue").count(1) == 0);
    CHECK(trie.search("calc").size() == 1);        // id 2 is still there
}

static void testHeap() {
    // "before" = "comes out first", so a < b gives ascending order.
    Heap<int> heap([](const int& a, const int& b) { return a < b; });
    for (int value : {5, 1, 4, 2, 3}) heap.push(value);

    std::string order;
    while (!heap.empty()) order += std::to_string(heap.pop());
    CHECK(order == "12345");

    // Push more than the initial capacity to exercise grow().
    Heap<int> big([](const int& a, const int& b) { return a < b; });
    for (int value = 100; value > 0; value--) big.push(value);
    CHECK(big.pop() == 1);
    CHECK(big.pop() == 2);
}

// ---------------------------------------------------------------------------
// Database + repositories
// ---------------------------------------------------------------------------
static User makeUser(const std::string& email, const std::string& name) {
    User user;
    user.email = email;
    user.name = name;
    user.passHash = "hash-placeholder";
    user.role = "user";
    return user;
}

static Item makeItem(int ownerId, const std::string& title, double price) {
    Item item;
    item.ownerId = ownerId;
    item.owner = "someone";
    item.type = "sell";
    item.title = title;
    item.description = "details";
    item.category = "Electronics";
    item.location = "Library";
    item.price = price;
    return item;
}

static void testRepositories() {
    Database db(":memory:");
    migrate(db);
    CHECK(db.userVersion() == SCHEMA_VERSION);
    CHECK(db.userVersion() == 6);

    UserRepo users(db);
    ItemRepo items(db);
    CHECK(users.count() == 0);
    CHECK(items.loadAll().empty());

    User user = makeUser("a@geu.ac.in", "Alice");
    users.insert(user);
    CHECK(user.id > 0);
    CHECK(user.createdAt > 0);
    CHECK(users.emailExists("a@geu.ac.in"));
    CHECK(users.findById(user.id).has_value());
    CHECK(users.findByEmail("a@geu.ac.in")->name == "Alice");
    CHECK(users.findById(user.id)->emailVerifiedAt == 0);
    users.markVerified(user.id, 12345);
    CHECK(users.findById(user.id)->emailVerifiedAt == 12345);
    CHECK(!users.findByEmail("nobody@geu.ac.in").has_value());

    // The UNIQUE constraint is the real guarantee, not the SELECT before it.
    bool duplicateRejected = false;
    try {
        User duplicate = makeUser("a@geu.ac.in", "Impostor");
        users.insert(duplicate);
    } catch (const DbError&) {
        duplicateRejected = true;
    }
    CHECK(duplicateRejected);
    CHECK(users.count() == 1);

    Item item = items.insert(makeItem(user.id, "Calculator", 250));
    CHECK(item.id > 0);
    CHECK(item.createdAt > 0);
    CHECK(items.loadAll().size() == 1);
    CHECK(items.loadAll()[0].owner == "Alice");   // JOIN fills the display name

    CHECK(items.setStatus(item.id, "closed"));
    CHECK(items.loadAll()[0].status == "closed");
    CHECK(!items.setStatus(99999, "closed"));      // no such row

    CHECK(items.remove(item.id));
    CHECK(items.loadAll().empty());

    // A transaction that never commits must leave nothing behind.
    const long long before = users.count();
    try {
        Transaction tx(db);
        User rolledBack = makeUser("rollback@geu.ac.in", "Rollback");
        users.insert(rolledBack);
        CHECK(users.count() == before + 1);   // visible inside the transaction
        throw std::runtime_error("simulated failure after the write");
    } catch (const std::runtime_error&) {
        // destructor of Transaction runs ROLLBACK
    }
    CHECK(users.count() == before);
    CHECK(!users.emailExists("rollback@geu.ac.in"));
}

// Store writes to the database first, then memory. A new Store must therefore
// see everything the old one saved - that is the "survives restart" test.
static void testStorePersistence() {
    const auto dir = tempDir("store");
    const std::string path = (dir / "campus.db").string();

    User owner = makeUser("owner@geu.ac.in", "Owner");
    User other = makeUser("other@geu.ac.in", "Other");
    int itemId = 0;
    {
        Database db(path);
        migrate(db);
        migrate(db);   // idempotent: a second call must not fail or duplicate
        UserRepo users(db);
        ItemRepo items(db);
        users.insert(owner);
        users.insert(other);

        Store store(items);
        Item saved = store.add(makeItem(owner.id, "Drafting board", 300));
        itemId = saved.id;
        CHECK(itemId > 0);
        CHECK(store.query("", "", "new", 20, 0).items.size() == 1);
        CHECK(store.close(itemId, other.id, false) == Store::Result::Forbidden);
        CHECK(store.close(itemId, owner.id, false) == Store::Result::Ok);
    }
    {
        // Same file, brand new objects: this is what a restart looks like.
        Database db(path);
        migrate(db);
        UserRepo users(db);
        ItemRepo items(db);
        Store store(items);
        CHECK(store.query("", "", "new", 20, 0).items.empty());  // closed notices are hidden
        CHECK(items.loadAll().size() == 1);
        CHECK(items.loadAll()[0].status == "closed");        // and the close was saved
        CHECK(items.loadAll()[0].owner == "Owner");
    }
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Part 1 JSON -> SQLite import
// ---------------------------------------------------------------------------
static void testLegacyImport() {
    const auto dir = tempDir("legacy");
    {
        std::ofstream users((dir / "users.json").string());
        users << R"({"nextId": 2, "users": [
            {"id": 1, "email": "old@geu.ac.in", "name": "Old Student",
             "passHash": "argon2-hash-string", "role": "user"}]})";
    }
    {
        std::ofstream items((dir / "items.json").string());
        items << R"({"nextId": 2, "items": [
            {"id": 1, "type": "sell", "title": "Old calculator", "description": "works",
             "category": "Electronics", "location": "Hostel C", "owner": "Old Student",
             "ownerId": 1, "price": 120, "createdAt": 1700000000, "status": "open"}]})";
    }

    Database db(":memory:");
    migrate(db);
    UserRepo users(db);
    ItemRepo items(db);

    const auto log = importLegacyJson(dir.string(), db, users, items);
    CHECK(log.size() == 2);
    CHECK(users.count() == 1);
    CHECK(items.loadAll().size() == 1);
    CHECK(items.loadAll()[0].owner == "Old Student");
    CHECK(users.findByEmail("old@geu.ac.in")->emailVerifiedAt > 0);
    CHECK(items.loadAll()[0].price == 120);
    CHECK(items.loadAll()[0].createdAt == 1700000000);

    // Second run: the tables are no longer empty, so nothing is imported twice.
    const auto secondLog = importLegacyJson(dir.string(), db, users, items);
    CHECK(secondLog.empty());
    CHECK(items.loadAll().size() == 1);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Auth: hashing, sessions, lockout, domain rule
// ---------------------------------------------------------------------------
static void testAuth() {
    Database db(":memory:");
    migrate(db);
    UserRepo users(db);
    SessionRepo sessions(db);
    RateLimiter limiter(db);
    Auth auth(users, sessions, limiter);

    User student;
    CHECK(auth.registerUser("Student@GEU.ac.in", "Student", "password123", "user", false, &student).empty());
    CHECK(student.id > 0);
    CHECK(student.email == "student@geu.ac.in");   // normalised

    CHECK(!auth.registerUser("student@geu.ac.in", "Copy", "password123", "user", false, nullptr).empty());
    CHECK(!auth.registerUser("short@geu.ac.in", "S", "short", "user", false, nullptr).empty());
    CHECK(!auth.registerUser("bad-email", "B", "password123", "user", false, nullptr).empty());

    std::string token;
    User loggedIn;
    CHECK(auth.login("student@geu.ac.in", "password123", token, loggedIn) == Auth::LoginResult::Ok);
    CHECK(!token.empty());
    CHECK(loggedIn.id == student.id);

    User fromToken;
    CHECK(auth.userForToken(token, fromToken));
    CHECK(fromToken.id == student.id);
    auth.logout(token);
    CHECK(!auth.userForToken(token, fromToken));

    CHECK(auth.login("student@geu.ac.in", "wrong-password", token, loggedIn) == Auth::LoginResult::Bad);
    CHECK(auth.login("nobody@geu.ac.in", "password123", token, loggedIn) == Auth::LoginResult::Bad);

    // A room of its own, so the failures above do not interfere.
    Database lockDb(":memory:");
    migrate(lockDb);
    UserRepo lockUsers(lockDb);
    SessionRepo lockSessions(lockDb);
    RateLimiter lockLimiter(lockDb);
    Auth lockAuth(lockUsers, lockSessions, lockLimiter);
    User lockUser;
    CHECK(lockAuth.registerUser("lock@geu.ac.in", "Lock", "correct-password", "user", false, &lockUser).empty());
    for (int attempt = 0; attempt < Auth::MAX_FAILURES; attempt++) {
        std::string t;
        User u;
        CHECK(lockAuth.login("lock@geu.ac.in", "wrong-password", t, u) == Auth::LoginResult::Bad);
    }
    {
        std::string t;
        User u;
        CHECK(lockAuth.login("lock@geu.ac.in", "correct-password", t, u) == Auth::LoginResult::Locked);
    }

    // The counters are rows in SQLite, so a restart does not clear the lockout.
    {
        RateLimiter restartedLimiter(lockDb);
        Auth restartedAuth(lockUsers, lockSessions, restartedLimiter);
        std::string t;
        User u;
        CHECK(restartedAuth.login("lock@geu.ac.in", "correct-password", t, u) == Auth::LoginResult::Locked);
    }

    // Per-IP limit: 20 failures from one network block that network, but a
    // different network with the correct password still works.
    Database ipDb(":memory:");
    migrate(ipDb);
    UserRepo ipUsers(ipDb);
    SessionRepo ipSessions(ipDb);
    RateLimiter ipLimiter(ipDb);
    Auth ipAuth(ipUsers, ipSessions, ipLimiter);
    User ipUser;
    CHECK(ipAuth.registerUser("ip@geu.ac.in", "IP User", "password123", "user", false, &ipUser).empty());
    for (int i = 0; i < 20; i++) {
        std::string t;
        User u;
        CHECK(ipAuth.login("nobody" + std::to_string(i) + "@geu.ac.in", "x", "203.0.113.9", t, u) ==
              Auth::LoginResult::Bad);
    }
    {
        std::string t;
        User u;
        CHECK(ipAuth.login("ip@geu.ac.in", "password123", "203.0.113.9", t, u) == Auth::LoginResult::Locked);
        CHECK(ipAuth.login("ip@geu.ac.in", "password123", "198.51.100.7", t, u) == Auth::LoginResult::Ok);
    }

    // Domain rule: only @geu.ac.in addresses are accepted when configured.
    Database domainDb(":memory:");
    migrate(domainDb);
    UserRepo domainUsers(domainDb);
    SessionRepo domainSessions(domainDb);
    RateLimiter domainLimiter(domainDb);
    Auth domainAuth(domainUsers, domainSessions, domainLimiter);
    domainAuth.setAllowedDomain("geu.ac.in");
    CHECK(domainAuth.registerUser("wrong@gmail.com", "Wrong", "password123", "user", true, nullptr) ==
          "use your @geu.ac.in email address");
    CHECK(domainAuth.registerUser("right@geu.ac.in", "Right", "password123", "user", true, nullptr).empty());
}


// ---------------------------------------------------------------------------
// Phase 2: sessions live in SQLite
// ---------------------------------------------------------------------------
static void testSessions() {
    Database db(":memory:");
    migrate(db);
    CHECK(db.userVersion() == 6);

    UserRepo users(db);
    SessionRepo sessions(db);
    RateLimiter limiter(db);
    Auth auth(users, sessions, limiter);

    User student;
    CHECK(auth.registerUser("sess@geu.ac.in", "Session User", "password123", "user", false, &student).empty());

    std::string tokenOne, tokenTwo;
    User loggedIn;
    CHECK(auth.login("sess@geu.ac.in", "password123", tokenOne, loggedIn) == Auth::LoginResult::Ok);
    CHECK(auth.login("sess@geu.ac.in", "password123", tokenTwo, loggedIn) == Auth::LoginResult::Ok);
    CHECK(tokenOne != tokenTwo);
    CHECK(sessions.countForUser(student.id) == 2);

    // The database stores only a 64-character SHA-256 hex digest, never the token.
    {
        Statement s = db.prepare("SELECT token_hash FROM sessions LIMIT 1");
        CHECK(s.step());
        const std::string stored = s.columnText(0);
        CHECK(stored.size() == 64);
        CHECK(stored != tokenOne && stored != tokenTwo);
    }

    User fromToken;
    CHECK(auth.userForToken(tokenOne, fromToken));
    CHECK(fromToken.id == student.id);

    // A brand-new Auth object on the same database is a server restart.
    {
        SessionRepo restartSessions(db);
        RateLimiter restartLimiter(db);
        Auth restarted(users, restartSessions, restartLimiter);
        User survivor;
        CHECK(restarted.userForToken(tokenOne, survivor));
        CHECK(survivor.id == student.id);
    }

    // Normal logout ends one session only.
    auth.logout(tokenOne);
    CHECK(!auth.userForToken(tokenOne, fromToken));
    CHECK(auth.userForToken(tokenTwo, fromToken));

    // "Log out everywhere" ends all of them.
    CHECK(auth.logoutAll(student.id) == 1);
    CHECK(!auth.userForToken(tokenTwo, fromToken));
    CHECK(sessions.countForUser(student.id) == 0);

    // An expired row is refused by the query even before the cleanup deletes it.
    const long long now = static_cast<long long>(std::time(nullptr));
    sessions.create(student.id, std::string(64, 'a'), now - 10);
    CHECK(!sessions.findUserId(std::string(64, 'a'), now).has_value());
    CHECK(sessions.pruneExpired(now) >= 1);
    CHECK(sessions.countForUser(student.id) == 0);

    // One account cannot accumulate unlimited sessions.
    for (int i = 0; i < 12; i++) sessions.create(student.id, "cap-token-" + std::to_string(i), now + 3600);
    CHECK(sessions.countForUser(student.id) == 10);

    // A Phase 1 database (schema version 1) is upgraded in place, and existing
    // accounts are treated as verified so the upgrade cannot lock anyone out.
    Database oldDb(":memory:");
    migrate(oldDb);
    UserRepo oldUsers(oldDb);
    User legacy = makeUser("legacy@geu.ac.in", "Legacy");
    oldUsers.insert(legacy);
    CHECK(oldUsers.findById(legacy.id)->emailVerifiedAt == 0);

    oldDb.setUserVersion(1);
    oldDb.exec("DROP TABLE sessions");
    oldDb.exec("DROP TABLE email_tokens");
    oldDb.exec("DROP TABLE rate_limits");
    oldDb.exec("DROP TABLE claims");
    oldDb.exec("DROP TABLE notifications");
    oldDb.exec("ALTER TABLE users DROP COLUMN email_verified_at");
    oldDb.exec("ALTER TABLE users DROP COLUMN banned_at");
    oldDb.exec("ALTER TABLE items DROP COLUMN proof");
    oldDb.exec("ALTER TABLE items DROP COLUMN contact");
    oldDb.exec("ALTER TABLE items DROP COLUMN image_mime");
    migrate(oldDb);

    CHECK(oldDb.userVersion() == 6);
    Statement tableCheck = oldDb.prepare(
        "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
        "AND name IN ('sessions','email_tokens','rate_limits','claims','notifications')");
    CHECK(tableCheck.step() && tableCheck.columnInt(0) == 5);
    CHECK(oldUsers.findById(legacy.id)->emailVerifiedAt > 0);
}


// ---------------------------------------------------------------------------
// Phase 3: email verification and password reset
// ---------------------------------------------------------------------------
class CapturingMailer : public Mailer {
public:
    struct Message {
        std::string to, name, subject, body;
    };
    std::vector<Message> messages;

    bool send(const std::string& toEmail, const std::string& toName,
              const std::string& subject, const std::string& body) override {
        messages.push_back({toEmail, toName, subject, body});
        return true;
    }
};

static std::string tokenFromLink(const std::string& body, const std::string& marker) {
    const size_t start = body.find(marker);
    if (start == std::string::npos) return "";
    const size_t begin = start + marker.size();
    const size_t end = body.find_first_of(" \r\n", begin);
    return body.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

static void testEmailVerification() {
    Database db(":memory:");
    migrate(db);
    UserRepo users(db);
    SessionRepo sessions(db);
    EmailTokenRepo tokens(db);
    RateLimiter limiter(db);
    Auth auth(users, sessions, limiter);
    CapturingMailer mailer;
    EmailService email(tokens, users, sessions, mailer, "http://test.local");

    User student;
    CHECK(auth.registerUser("mail@geu.ac.in", "Mail User", "password123", "user", false, &student).empty());
    CHECK(student.emailVerifiedAt == 0);

    CHECK(email.sendVerification(student) == EmailService::SendResult::Sent);
    CHECK(mailer.messages.size() == 1);
    CHECK(mailer.messages[0].subject.find("Verify") != std::string::npos);
    const std::string verifyToken = tokenFromLink(mailer.messages[0].body, "/?verify=");
    CHECK(!verifyToken.empty());
    CHECK(verifyToken.size() == 64);   // 32 random bytes as hex

    // Immediate second request is refused by the cooldown.
    CHECK(email.sendVerification(student) == EmailService::SendResult::TooSoon);

    CHECK(email.verify(verifyToken) == EmailService::VerifyResult::Ok);
    CHECK(users.findById(student.id)->emailVerifiedAt > 0);
    // Single use: the same link cannot be replayed.
    CHECK(email.verify(verifyToken) == EmailService::VerifyResult::Invalid);
    CHECK(email.verify("not-a-token") == EmailService::VerifyResult::Invalid);

    // Password reset: create a login session first, then check it dies.
    std::string sessionToken;
    User loggedIn;
    CHECK(auth.login("mail@geu.ac.in", "password123", sessionToken, loggedIn) == Auth::LoginResult::Ok);

    CHECK(email.sendPasswordReset(*users.findById(student.id)) == EmailService::SendResult::Sent);
    CHECK(mailer.messages.size() == 2);
    const std::string resetToken = tokenFromLink(mailer.messages[1].body, "/?reset=");
    CHECK(!resetToken.empty());

    // A weak password is rejected without burning the link.
    CHECK(email.resetPassword(resetToken, "short") == EmailService::ResetResult::WeakPassword);
    CHECK(email.resetPassword(resetToken, "newpassword123") == EmailService::ResetResult::Ok);

    User afterReset;
    CHECK(!auth.userForToken(sessionToken, afterReset));   // every old session revoked
    CHECK(auth.login("mail@geu.ac.in", "password123", sessionToken, afterReset) == Auth::LoginResult::Bad);
    CHECK(auth.login("mail@geu.ac.in", "newpassword123", sessionToken, afterReset) == Auth::LoginResult::Ok);
    CHECK(email.resetPassword(resetToken, "anotherpassword1") == EmailService::ResetResult::InvalidToken);
}


// ---------------------------------------------------------------------------
// Phase 4: rate limiter
// ---------------------------------------------------------------------------
static void testRateLimiter() {
    Database db(":memory:");
    migrate(db);
    RateLimiter limiter(db);
    const long long now = 1000000;

    CHECK(limiter.count("key", 60, now) == 0);
    limiter.record("key", 60, now);
    limiter.record("key", 60, now);
    CHECK(limiter.count("key", 60, now) == 2);

    // After the window passes the count is zero, and the next hit starts over.
    CHECK(limiter.count("key", 60, now + 61) == 0);
    limiter.record("key", 60, now + 61);
    CHECK(limiter.count("key", 60, now + 61) == 1);

    limiter.clear("key");
    CHECK(limiter.count("key", 60, now + 61) == 0);

    // Independent keys do not interfere.
    limiter.record("a", 60, now);
    limiter.record("b", 60, now);
    CHECK(limiter.count("a", 60, now) == 1);
    CHECK(limiter.count("b", 60, now) == 1);

    limiter.prune(now + 3600);
}


// ---------------------------------------------------------------------------
// Phase 6: lost & found claims
// ---------------------------------------------------------------------------
static void testClaims() {
    Database db(":memory:");
    migrate(db);
    UserRepo users(db);
    ItemRepo items(db);
    ClaimRepo claims(db);

    User owner = makeUser("claim-owner@geu.ac.in", "Owner");
    User claimant = makeUser("claimant@geu.ac.in", "Claimer");
    User other = makeUser("other-claimant@geu.ac.in", "Other");
    users.insert(owner);
    users.insert(claimant);
    users.insert(other);

    Item lost = makeItem(owner.id, "Blue bottle", 0);
    lost.type = "lost";
    lost.proof = "cat sticker on the base";
    lost.contact = "Hostel C room 12";
    lost = items.insert(lost);
    CHECK(items.loadAll()[0].proof == "cat sticker on the base");
    CHECK(items.loadAll()[0].contact == "Hostel C room 12");

    CHECK(claims.countPending(lost.id) == 0);
    const int first = claims.create(lost.id, claimant.id, "cat sticker", "found it", "phone 1");
    CHECK(first > 0);
    CHECK(claims.exists(lost.id, claimant.id));
    CHECK(claims.create(lost.id, claimant.id, "again", "", "phone 2") < 0);  // UNIQUE constraint

    const int second = claims.create(lost.id, other.id, "the sticker", "", "phone 3");
    CHECK(second > 0);
    CHECK(claims.countPending(lost.id) == 2);

    std::vector<Claim> forOwner = claims.listForItem(lost.id);
    CHECK(forOwner.size() == 2);
    CHECK(forOwner[0].claimantName == "Claimer");
    CHECK(forOwner[0].contact == "phone 1");

    std::vector<MyClaim> mine = claims.listForClaimant(claimant.id);
    CHECK(mine.size() == 1);
    CHECK(mine[0].status == "pending");
    CHECK(mine[0].ownerContact.empty());   // hidden before approval
    CHECK(mine[0].ownerName == "Owner");

    // Approving one claim closes the item and rejects the other pending claim.
    CHECK(claims.decide(lost.id, first, owner.id, true, 100).status == DecideResult::Status::Ok);
    CHECK(items.loadAll()[0].status == "closed");

    std::vector<Claim> after = claims.listForItem(lost.id);
    CHECK(after.size() == 2);
    CHECK(after[0].status == "approved");
    CHECK(after[1].status == "rejected");

    std::vector<MyClaim> afterMine = claims.listForClaimant(claimant.id);
    CHECK(afterMine[0].status == "approved");
    CHECK(afterMine[0].ownerContact == "Hostel C room 12");   // revealed after approval

    // A decided claim cannot be decided again.
    CHECK(claims.decide(lost.id, first, owner.id, true, 101).status == DecideResult::Status::NotFound);
}


// ---------------------------------------------------------------------------
// Phase 7: rehashing, inverted index, pagination / top-K
// ---------------------------------------------------------------------------
static void testHashTableGrowth() {
    HashTable table;
    CHECK(table.bucketCount() == 101);
    CHECK(table.size() == 0);

    // 1000 inserts must trigger rehash(es): a fixed 101-bucket table would now
    // have chains of ~10 items.
    for (int i = 1; i <= 1000; i++) {
        Item item;
        item.id = i;
        item.title = "notice";
        table.put(item);
    }
    CHECK(table.size() == 1000);
    CHECK(table.bucketCount() > 101);
    CHECK(table.get(1) != nullptr);
    CHECK(table.get(500) != nullptr);
    CHECK(table.get(1000) != nullptr);

    CHECK(table.remove(500));
    CHECK(table.get(500) == nullptr);
    CHECK(table.size() == 999);

    Item renamed;
    renamed.id = 1;
    renamed.title = "renamed";
    table.put(renamed);              // replaces, does not add
    CHECK(table.size() == 999);
    CHECK(table.get(1)->title == "renamed");
}

static void testInvertedIndex() {
    InvertedIndex index;
    index.insert("Blue calculator", 1);
    index.insert("Calculator case", 2);
    index.insert("Blue wallet", 3);
    CHECK(index.wordCount() == 4);   // blue, calculator, case, wallet

    CHECK(index.find("calculator").size() == 2);
    CHECK(index.find("Blue").size() == 2);      // cleaned to lower-case
    CHECK(index.find("wall").empty());          // exact words only; the trie does prefixes

    index.erase("Blue wallet", 3);
    CHECK(index.find("wallet").empty());        // empty bucket removed
    CHECK(index.wordCount() == 3);
}

static void testTokeniser() {
    CHECK(cleanWord("Calculator,") == "calculator");
    std::vector<std::string> words = splitWords("  Blue  CALCULATOR-case!  ");
    CHECK(words.size() == 3);
    CHECK(words[0] == "blue");
    CHECK(words[1] == "calculator");
    CHECK(words[2] == "case");
}

static void testSearchAndPagination() {
    Database db(":memory:");
    migrate(db);
    UserRepo users(db);
    ItemRepo items(db);
    User owner = makeUser("index-owner@geu.ac.in", "Index Owner");
    users.insert(owner);

    auto add = [&](const std::string& title, const std::string& type, double price, long long createdAt) {
        Item item = makeItem(owner.id, title, price);
        item.type = type;
        item.createdAt = createdAt;
        return items.insert(item);
    };
    add("Blue calculator", "sell", 300, 100);
    add("Calculator case", "sell", 100, 200);
    add("Blue wallet", "sell", 50, 300);
    add("Desk lamp", "sell", 200, 400);
    add("Lost keys", "lost", 0, 500);

    Store store(items);

    // Newest first, paged.
    Store::QueryPage first = store.query("", "", "new", 2, 0);
    CHECK(first.items.size() == 2);
    CHECK(first.total == 5);
    CHECK(first.items.front().title == "Lost keys");
    Store::QueryPage second = store.query("", "", "new", 2, 2);
    CHECK(second.items.size() == 2);
    CHECK(second.items.front().title == "Blue wallet");
    Store::QueryPage last = store.query("", "", "new", 2, 4);
    CHECK(last.items.size() == 1);
    CHECK(last.items.front().title == "Blue calculator");
    CHECK(store.query("", "", "new", 2, 5).items.empty());   // past the end

    // Cheapest first.
    CHECK(store.query("", "", "cheap", 3, 0).items.front().price == 0);

    // Exact word (inverted index) and prefix (trie) both work.
    CHECK(store.query("calculator", "", "new", 20, 0).total == 2);
    CHECK(store.query("calc", "", "new", 20, 0).total == 2);
    CHECK(store.query("blue", "", "new", 20, 0).total == 2);
    // Two words are ANDed.
    CHECK(store.query("blue calc", "", "new", 20, 0).total == 1);
    CHECK(store.query("wallet", "", "new", 20, 0).total == 1);
    CHECK(store.query("nosuchword", "", "new", 20, 0).total == 0);

    // Type filter is applied before counting the total.
    CHECK(store.query("", "lost", "new", 20, 0).total == 1);
    CHECK(store.query("", "sell", "new", 20, 0).total == 4);
}

// ---------------------------------------------------------------------------
// Phase 8: mail queue, images, notifications, admin banning
// ---------------------------------------------------------------------------
static void testMailQueue() {
    // The CapturingMailer is defined earlier in this file (Phase 3 section).
    CapturingMailer inner;
    {
        MailQueue queue(inner);
        queue.push({"a@test", "A", "Subject 1", "Body 1"});
        queue.push({"b@test", "B", "Subject 2", "Body 2"});
        // waitIdle blocks until the worker has delivered both messages.
        CHECK(queue.waitIdle(std::chrono::milliseconds(2000)));
        CHECK(inner.messages.size() == 2);
        CHECK(inner.messages[0].subject == "Subject 1");
        CHECK(inner.messages[1].to == "b@test");
        // pending() should be 0 when idle.
        CHECK(queue.pending() == 0);
    }
    // After the destructor the worker is stopped.

    // MailQueue also satisfies the Mailer interface.
    CapturingMailer inner2;
    MailQueue queue2(inner2);
    Mailer& asMailer = queue2;
    asMailer.send("c@test", "C", "Subject 3", "Body 3");
    CHECK(queue2.waitIdle(std::chrono::milliseconds(2000)));
    CHECK(inner2.messages.size() == 1);
    CHECK(inner2.messages[0].subject == "Subject 3");
}

static void testImageSniffing() {
    // JPEG: starts with FF D8 FF
    std::string jpeg;
    jpeg += '\xFF'; jpeg += '\xD8'; jpeg += '\xFF'; jpeg += '\xE0';
    jpeg += "rest of file";
    auto r = sniffImage(jpeg);
    CHECK(r.has_value());
    CHECK(r->mime == "image/jpeg");
    CHECK(r->ext == ".jpg");

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    std::string png;
    png += '\x89'; png += 'P'; png += 'N'; png += 'G';
    png += '\r'; png += '\n'; png += '\x1a'; png += '\n';
    png += "data";
    r = sniffImage(png);
    CHECK(r.has_value());
    CHECK(r->mime == "image/png");

    // GIF
    r = sniffImage("GIF89aXXXX");
    CHECK(r.has_value());
    CHECK(r->mime == "image/gif");

    // WebP: "RIFF" + 4 bytes + "WEBP"
    std::string webp = "RIFF";
    webp += '\x00'; webp += '\x00'; webp += '\x00'; webp += '\x00';
    webp += "WEBP";
    webp += "rest";
    r = sniffImage(webp);
    CHECK(r.has_value());
    CHECK(r->mime == "image/webp");

    // Unknown / too short
    CHECK(!sniffImage("hello world").has_value());
    CHECK(!sniffImage("").has_value());

    CHECK(extForMime("image/jpeg") == ".jpg");
    CHECK(extForMime("image/png") == ".png");
    CHECK(extForMime("image/gif") == ".gif");
    CHECK(extForMime("image/webp") == ".webp");
    CHECK(extForMime("application/pdf") == "");
}

static void testNotifications() {
    Database db(":memory:");
    migrate(db);
    UserRepo users(db);
    ItemRepo items(db);
    NotificationRepo notifs(db);

    User u = makeUser("notif@geu.ac.in", "Notif User");
    users.insert(u);
    // Create real items so the FK constraint on notifications.item_id is satisfied.
    Item it1 = items.insert(makeItem(u.id, "Bottle", 0));
    Item it2 = items.insert(makeItem(u.id, "Keys", 0));

    CHECK(notifs.unreadCount(u.id) == 0);
    CHECK(notifs.listFor(u.id, 10).empty());

    notifs.create(u.id, "claim", it1.id, "someone claimed your notice");
    notifs.create(u.id, "approved", it2.id, "your claim was approved");
    CHECK(notifs.unreadCount(u.id) == 2);

    std::vector<Notification> list = notifs.listFor(u.id, 10);
    CHECK(list.size() == 2);
    CHECK(list[0].kind == "approved");   // newest first (same second, higher id)
    CHECK(list[0].read == false);
    CHECK(list[1].kind == "claim");

    notifs.markAllRead(u.id, 9999);
    CHECK(notifs.unreadCount(u.id) == 0);
    CHECK(notifs.listFor(u.id, 10)[0].read == true);

    // Limit is respected.
    for (int i = 0; i < 5; i++) notifs.create(u.id, "claim", it1.id, "extra");
    CHECK(notifs.listFor(u.id, 3).size() == 3);
}

static void testUserBanning() {
    Database db(":memory:");
    migrate(db);
    UserRepo users(db);
    SessionRepo sessions(db);
    RateLimiter limiter(db);
    Auth auth(users, sessions, limiter);

    User u;
    CHECK(auth.registerUser("ban@geu.ac.in", "Ban Target", "password123", "user", false, &u).empty());
    std::string token;
    User loggedIn;
    CHECK(auth.login("ban@geu.ac.in", "password123", token, loggedIn) == Auth::LoginResult::Ok);
    CHECK(auth.userForToken(token, loggedIn));

    // Ban the user.
    users.ban(u.id, 12345);
    CHECK(users.findById(u.id)->bannedAt == 12345);

    // Existing sessions are rejected.
    User out;
    CHECK(!auth.userForToken(token, out));
    // Login is refused.
    std::string t2;
    CHECK(auth.login("ban@geu.ac.in", "password123", t2, out) == Auth::LoginResult::Bad);

    // Unban restores access.
    users.unban(u.id);
    CHECK(users.findById(u.id)->bannedAt == 0);
    // Need a new session since the old one was not removed by ban.
    std::string t3;
    CHECK(auth.login("ban@geu.ac.in", "password123", t3, out) == Auth::LoginResult::Ok);

    // adminList returns all users.
    std::vector<User> all = users.adminList();
    CHECK(!all.empty());
    bool found = false;
    for (const User& usr : all) {
        if (usr.id == u.id) { found = true; break; }
    }
    CHECK(found);
}

static void testAdminStats() {
    Database db(":memory:");
    migrate(db);
    UserRepo users(db);
    ItemRepo items(db);
    SessionRepo sessions(db);
    ClaimRepo claims(db);

    User u = makeUser("stats@geu.ac.in", "Stats User");
    users.insert(u);
    users.markVerified(u.id, 1000);

    CHECK(users.count() == 1);
    CHECK(users.countSince(500) == 1);   // created after t=500? depends on insert; use 0
    CHECK(users.countSince(0) == 1);

    Item sell = makeItem(u.id, "Sell item", 100); sell.type = "sell";
    Item lend = makeItem(u.id, "Lend item", 0);  lend.type = "lend";
    Item lost = makeItem(u.id, "Lost item", 0);  lost.type = "lost";
    sell = items.insert(sell);
    lend = items.insert(lend);
    lost = items.insert(lost);

    CHECK(items.countOpen() == 3);
    CHECK(items.countClosed() == 0);
    CHECK(items.countByType("sell") == 1);
    CHECK(items.countByType("lend") == 1);
    CHECK(items.countByType("lost") == 1);
    CHECK(items.countByType("found") == 0);

    items.setStatus(sell.id, "closed");
    CHECK(items.countOpen() == 2);
    CHECK(items.countClosed() == 1);

    // Claims stats.
    User claimant = makeUser("claimant-stats@geu.ac.in", "Claimant");
    users.insert(claimant);
    claims.create(lost.id, claimant.id, "answer", "", "contact");
    CHECK(claims.countByStatus("pending") == 1);
    CHECK(claims.countByStatus("approved") == 0);

    // Admin list of claims.
    std::vector<AdminClaim> all = claims.listAll(10, 0);
    CHECK(all.size() == 1);
    CHECK(all[0].itemTitle == "Lost item");

    // Session count.
    const long long now = static_cast<long long>(std::time(nullptr));
    sessions.create(u.id, "hash1", now + 3600);
    sessions.create(u.id, "hash2", now + 3600);
    CHECK(sessions.countActive(now) == 2);
    CHECK(sessions.countActive(now + 7200) == 0);  // both expired
}

// ---------------------------------------------------------------------------
int main() {
    if (sodium_init() < 0) {
        std::fprintf(stderr, "libsodium failed to initialise\n");
        return 2;
    }

    testHashTable();
    testTrie();
    testHeap();
    testRepositories();
    testStorePersistence();
    testLegacyImport();
    testAuth();
    testSessions();
    testEmailVerification();
    testRateLimiter();
    testClaims();
    testHashTableGrowth();
    testInvertedIndex();
    testTokeniser();
    testSearchAndPagination();
    testMailQueue();
    testImageSniffing();
    testNotifications();
    testUserBanning();
    testAdminStats();

    std::printf("%d checks, %d failed\n", checksRun, checksFailed);
    return checksFailed == 0 ? 0 : 1;
}
