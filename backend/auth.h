#pragma once
// Accounts, password hashing and login sessions.
//
// - Passwords are hashed with Argon2id (libsodium crypto_pwhash_str). The stored string
//   contains the random salt and the cost settings, so nothing else needs to be saved.
// - A session is a random 256-bit token kept in a hash map (token -> user id + expiry).
//   The browser only ever holds the token, in an HttpOnly cookie.
// - Roles live on the server. Nobody can make themselves admin from the browser.
#include <sodium.h>
#include <cctype>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include "storage.h"

struct User {
    int id = 0;
    std::string email;
    std::string name;
    std::string passHash;
    std::string role = "user";   // "user" or "admin"
};

class Auth {
public:
    enum class LoginResult { Ok, Bad, Locked };

    static const long long SESSION_SECONDS = 24 * 60 * 60;
    static const int MAX_FAILURES = 5;            // failed logins allowed per email...
    static const long long LOCK_SECONDS = 15 * 60; // ...within this window

private:
    struct Session {
        int userId;
        long long expiresAt;
    };
    struct Attempts {
        int failures = 0;
        long long windowStart = 0;
    };

    std::string path;
    std::string allowedDomain;                          // e.g. "geu.ac.in"; empty = any domain
    std::map<int, User> users;                          // id -> user
    std::unordered_map<std::string, int> byEmail;       // email -> id
    std::unordered_map<std::string, Session> sessions;  // token -> session
    std::unordered_map<std::string, Attempts> attempts; // email -> recent failed logins
    int nextId = 1;
    std::string dummyHash;   // verified against when the email is unknown, so timing does not reveal which emails exist
    std::mutex mtx;

    static long long now() { return (long long)std::time(nullptr); }

    static std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) a++;
        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
        return s.substr(a, b - a);
    }

    static size_t utf8Length(const std::string& s) {
        size_t n = 0;
        for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
        return n;
    }

    static bool hasControlChars(const std::string& s) {
        for (unsigned char c : s) if (c < 0x20 || c == 0x7F) return true;
        return false;
    }

    static std::string hashPassword(const std::string& password) {
        char out[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str(out, password.c_str(), password.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            throw std::runtime_error("out of memory while hashing password");
        }
        return out;
    }

    static bool verifyPassword(const std::string& hash, const std::string& password) {
        return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
    }

    static std::string newToken() {
        unsigned char raw[32];
        randombytes_buf(raw, sizeof raw);
        char hex[sizeof raw * 2 + 1];
        sodium_bin2hex(hex, sizeof hex, raw, sizeof raw);
        return hex;
    }

    void saveLocked() {
        json list = json::array();
        for (const auto& p : users) {
            const User& u = p.second;
            list.push_back({{"id", u.id}, {"email", u.email}, {"name", u.name},
                            {"passHash", u.passHash}, {"role", u.role}});
        }
        // ownerOnly = true: the file holds password hashes, so only the server's OS user may read it.
        writeJsonFileAtomic(path, json{{"nextId", nextId}, {"users", list}}, true);
    }

    void pruneExpiredLocked() {
        long long t = now();
        if (sessions.size() > 1000) {
            for (auto it = sessions.begin(); it != sessions.end();) {
                if (it->second.expiresAt <= t) it = sessions.erase(it); else ++it;
            }
        }
        if (attempts.size() > 1000) {
            for (auto it = attempts.begin(); it != attempts.end();) {
                if (t - it->second.windowStart >= LOCK_SECONDS) it = attempts.erase(it); else ++it;
            }
        }
    }

public:
    explicit Auth(std::string file) : path(std::move(file)) {
        dummyHash = hashPassword("not-a-real-password");
        json data;
        if (!readJsonFile(path, data)) return;
        nextId = data.value("nextId", 1);
        for (const json& j : data.at("users")) {
            User u;
            u.id = j.at("id");
            u.email = j.at("email");
            u.name = j.at("name");
            u.passHash = j.at("passHash");
            u.role = j.value("role", "user");
            users[u.id] = u;
            byEmail[u.email] = u.id;
            if (u.id >= nextId) nextId = u.id + 1;
        }
    }

    void setAllowedDomain(const std::string& domain) {
        std::lock_guard<std::mutex> lock(mtx);
        allowedDomain = normalizeEmail(domain);
    }

    static std::string normalizeEmail(const std::string& email) {
        std::string e = trim(email);
        for (char& c : e) c = (char)std::tolower((unsigned char)c);
        return e;
    }

    // Returns an error message, or "" on success (and fills *out).
    std::string registerUser(const std::string& emailIn, const std::string& nameIn, const std::string& password,
                             const std::string& role, bool enforceDomain, User* out) {
        std::string email = normalizeEmail(emailIn);
        std::string name = trim(nameIn);

        size_t at = email.find('@');
        bool emailOk = email.size() >= 3 && email.size() <= 100 && at != std::string::npos && at > 0 &&
                       email.find('@', at + 1) == std::string::npos &&
                       email.find('.', at) != std::string::npos && email.back() != '.' &&
                       email.find_first_of(" \t\r\n") == std::string::npos;
        if (!emailOk) return "enter a valid email address";
        if (name.empty() || utf8Length(name) > 40 || hasControlChars(name)) return "name must be 1 to 40 characters";
        if (password.size() < 8) return "password must be at least 8 characters";
        if (password.size() > 128) return "password must be at most 128 characters";

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (enforceDomain && !allowedDomain.empty()) {
                std::string suffix = "@" + allowedDomain;
                if (email.size() <= suffix.size() || email.compare(email.size() - suffix.size(), suffix.size(), suffix) != 0) {
                    return "use your " + suffix + " email address";
                }
            }
            if (byEmail.count(email)) return "an account with this email already exists";
        }

        // Hashing is deliberately slow (~50-100 ms, 64 MB), so do it without holding the lock.
        std::string hash = hashPassword(password);

        std::lock_guard<std::mutex> lock(mtx);
        if (byEmail.count(email)) return "an account with this email already exists";  // re-check: another request may have won
        User u;
        u.id = nextId++;
        u.email = email;
        u.name = name;
        u.passHash = hash;
        u.role = role;
        users[u.id] = u;
        byEmail[email] = u.id;
        saveLocked();
        if (out) *out = u;
        return "";
    }

    LoginResult login(const std::string& emailIn, const std::string& password, std::string& tokenOut, User& userOut) {
        std::string email = normalizeEmail(emailIn);
        std::string hash;
        int userId = 0;
        {
            std::lock_guard<std::mutex> lock(mtx);
            pruneExpiredLocked();
            Attempts& a = attempts[email];
            if (now() - a.windowStart >= LOCK_SECONDS) a = Attempts{0, now()};
            if (a.failures >= MAX_FAILURES) return LoginResult::Locked;
            a.failures++;   // count the attempt before checking, so parallel guesses cannot slip past the limit

            auto it = byEmail.find(email);
            if (it != byEmail.end()) {
                userId = it->second;
                hash = users[userId].passHash;
            }
        }

        // Always run one hash check, even for an unknown email, so the response time is the same.
        bool ok = verifyPassword(userId ? hash : dummyHash, password) && userId != 0;
        if (!ok) return LoginResult::Bad;

        std::lock_guard<std::mutex> lock(mtx);
        attempts.erase(email);
        auto it = users.find(userId);
        if (it == users.end()) return LoginResult::Bad;
        tokenOut = newToken();
        sessions[tokenOut] = Session{userId, now() + SESSION_SECONDS};
        userOut = it->second;
        return LoginResult::Ok;
    }

    // Starts a session for a user who just registered.
    std::string startSession(int userId) {
        std::lock_guard<std::mutex> lock(mtx);
        pruneExpiredLocked();
        std::string token = newToken();
        sessions[token] = Session{userId, now() + SESSION_SECONDS};
        return token;
    }

    bool userForToken(const std::string& token, User& out) {
        if (token.empty()) return false;
        std::lock_guard<std::mutex> lock(mtx);
        auto it = sessions.find(token);
        if (it == sessions.end()) return false;
        if (it->second.expiresAt <= now()) {
            sessions.erase(it);
            return false;
        }
        auto u = users.find(it->second.userId);
        if (u == users.end()) return false;
        out = u->second;
        return true;
    }

    void logout(const std::string& token) {
        std::lock_guard<std::mutex> lock(mtx);
        sessions.erase(token);
    }

    // Creates the first admin from server configuration. Does nothing if an admin already exists.
    // Returns a message for the server log.
    std::string ensureAdmin(const std::string& email, const std::string& name, const std::string& password) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (const auto& p : users) {
                if (p.second.role == "admin") return "";
            }
            if (email.empty()) {
                return "WARNING: no admin account exists. Set CC_ADMIN_EMAIL, CC_ADMIN_NAME and CC_ADMIN_PASSWORD and restart.";
            }
            auto it = byEmail.find(normalizeEmail(email));
            if (it != byEmail.end()) {
                users[it->second].role = "admin";
                saveLocked();
                return "Promoted existing account " + normalizeEmail(email) + " to admin (its password is unchanged).";
            }
        }
        std::string err = registerUser(email, name.empty() ? "Admin" : name, password, "admin", false, nullptr);
        if (!err.empty()) return "WARNING: could not create admin: " + err;
        return "Created admin account " + normalizeEmail(email) + ".";
    }
};
