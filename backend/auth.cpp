// ---------------------------------------------------------------------------
// auth.cpp - password hashing rules, login lockout and session start/stop.
//
// Hashing lives in password.h and token generation/hashing in secrets.h, so
// this file is only about the account rules. That split matters because
// EmailService (password reset) needs the same primitives without needing Auth.
// ---------------------------------------------------------------------------
#include "auth.h"

#include <cctype>
#include <ctime>
#include <optional>

#include "password.h"
#include "secrets.h"

namespace {

const int LOGIN_EMAIL_LIMIT = 5;          // failures per email inside the window
const int LOGIN_IP_LIMIT = 20;            // failures per IP inside the window
const long long LOGIN_WINDOW_SECONDS = 15 * 60;

long long now() { return static_cast<long long>(std::time(nullptr)); }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

// Counts characters (not bytes), so a limit of 60 means 60 characters even for
// non-ASCII text.
size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) n++;
    }
    return n;
}

bool hasControlChars(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
Auth::Auth(UserRepo& users, SessionRepo& sessions, RateLimiter& rateLimiter)
    : users_(users), sessions_(sessions), rateLimiter_(rateLimiter) {
    // Building the dummy hash at start-up costs one Argon2 run, and from then on
    // a login for an unknown email takes the same time as a real one.
    dummyHash = hashPassword("not-a-real-password");
    // Drop rows left over from before the last restart/shutdown.
    sessions_.pruneExpired(now());
}

// Lower-cases and trims, so " Student@GEU.ac.in " and "student@geu.ac.in" are
// the same account. Public static so /api/forgot can normalise the same way.
std::string Auth::normalizeEmail(const std::string& email) {
    std::string e = trim(email);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

void Auth::setAllowedDomain(const std::string& domain) {
    std::lock_guard<std::mutex> lock(mtx);
    allowedDomain = normalizeEmail(domain);
}


std::string Auth::registerUser(const std::string& emailIn, const std::string& nameIn,
                               const std::string& password, const std::string& role,
                               bool enforceDomain, User* out) {
    std::string email = normalizeEmail(emailIn);
    std::string name = trim(nameIn);

    const size_t at = email.find('@');
    const bool emailOk = email.size() >= 3 && email.size() <= 100 && at != std::string::npos &&
                         at > 0 && email.find('@', at + 1) == std::string::npos &&
                         email.find('.', at) != std::string::npos && email.back() != '.' &&
                         email.find_first_of(" \t\r\n") == std::string::npos;
    if (!emailOk) return "enter a valid email address";
    if (name.empty() || utf8Length(name) > 40 || hasControlChars(name)) return "name must be 1 to 40 characters";
    if (password.size() < 8) return "password must be at least 8 characters";
    if (password.size() > 128) return "password must be at most 128 characters";

    {
        std::lock_guard<std::mutex> lock(mtx);
        if (enforceDomain && !allowedDomain.empty()) {
            const std::string suffix = "@" + allowedDomain;
            if (email.size() <= suffix.size() ||
                email.compare(email.size() - suffix.size(), suffix.size(), suffix) != 0) {
                return "use your " + suffix + " email address";
            }
        }
        if (users_.emailExists(email)) return "an account with this email already exists";
    }

    // Hashing is deliberately slow (~50-100 ms, 64 MB), so do it without holding the lock.
    const std::string hash = hashPassword(password);

    std::lock_guard<std::mutex> lock(mtx);
    if (users_.emailExists(email)) return "an account with this email already exists";  // re-check under the lock

    User user;
    user.email = email;
    user.name = name;
    user.passHash = hash;
    user.role = role;
    users_.insert(user);
    if (out) *out = user;
    return "";
}

Auth::LoginResult Auth::login(const std::string& emailIn, const std::string& password,
                              const std::string& clientIp, std::string& tokenOut, User& userOut) {
    const std::string email = normalizeEmail(emailIn);
    const std::string emailKey = "login-email:" + email;
    const std::string ipKey = clientIp.empty() ? std::string() : "login-ip:" + clientIp;
    const long long t = now();

    // Check before doing any expensive hashing: a locked-out caller costs one
    // indexed SELECT, not an Argon2 run.
    if (rateLimiter_.count(emailKey, LOGIN_WINDOW_SECONDS, t) >= LOGIN_EMAIL_LIMIT) {
        return LoginResult::Locked;
    }
    if (!ipKey.empty() && rateLimiter_.count(ipKey, LOGIN_WINDOW_SECONDS, t) >= LOGIN_IP_LIMIT) {
        return LoginResult::Locked;
    }

    // The database read happens outside any lock: it is a fast indexed lookup.
    std::optional<User> found = users_.findByEmail(email);
    // Always verify one hash, even for an unknown email, so the response time
    // does not reveal which emails have accounts.
    const bool ok = verifyPassword(found ? found->passHash : dummyHash, password) && found.has_value();
    if (!ok) {
        // Count the failure against the account and against the network.
        rateLimiter_.record(emailKey, LOGIN_WINDOW_SECONDS, now());
        if (!ipKey.empty()) rateLimiter_.record(ipKey, LOGIN_WINDOW_SECONDS, now());
        return LoginResult::Bad;
    }
    // A success forgives the account, but not the network: an attacker who knows
    // one password must not be able to reset the IP counter by logging into it.
    // Phase 8: reject banned accounts the same way as a bad password, to avoid
    // revealing that the account exists but is banned.
    if (found->bannedAt != 0) {
        rateLimiter_.record(emailKey, LOGIN_WINDOW_SECONDS, now());
        if (!ipKey.empty()) rateLimiter_.record(ipKey, LOGIN_WINDOW_SECONDS, now());
        return LoginResult::Bad;
    }
    rateLimiter_.clear(emailKey);
    sessions_.pruneExpired(now());
    tokenOut = startSession(found->id);
    userOut = *found;
    return LoginResult::Ok;
}

std::string Auth::startSession(int userId) {
    const std::string token = randomToken();
    // Only the hash is stored: a copy of the database cannot be used to log in.
    sessions_.create(userId, hashToken(token), now() + SESSION_SECONDS);
    return token;
}

bool Auth::userForToken(const std::string& token, User& out) {
    if (token.empty()) return false;
    // The lookup and the expiry check are a single indexed query, so an expired
    // session can never be accepted even before the cleanup deletes the row.
    std::optional<int> userId = sessions_.findUserId(hashToken(token), now());
    if (!userId) return false;
    std::optional<User> user = users_.findById(*userId);
    if (!user) return false;
    // Phase 8: a banned user's existing sessions are rejected on the next request.
    if (user->bannedAt != 0) return false;
    out = *user;
    return true;
}

void Auth::logout(const std::string& token) {
    sessions_.remove(hashToken(token));
}

int Auth::logoutAll(int userId) {
    return sessions_.removeAllForUser(userId);
}

std::string Auth::ensureAdmin(const std::string& email, const std::string& name,
                              const std::string& password) {
    if (users_.countAdmins() > 0) return "";
    if (email.empty()) {
        return "WARNING: no admin account exists. Set CC_ADMIN_EMAIL, CC_ADMIN_NAME and CC_ADMIN_PASSWORD and restart.";
    }
    const std::string normalised = normalizeEmail(email);
    std::optional<User> existing = users_.findByEmail(normalised);
    if (existing) {
        users_.promoteToAdmin(existing->id);
        // The admin came from server configuration, so there is no email to verify.
        users_.markVerified(existing->id, now());
        return "Promoted existing account " + normalised + " to admin (its password is unchanged).";
    }
    const std::string err = registerUser(email, name.empty() ? "Admin" : name, password, "admin", false, nullptr);
    if (!err.empty()) return "WARNING: could not create admin: " + err;
    std::optional<User> created = users_.findByEmail(normalised);
    if (created) users_.markVerified(created->id, now());
    return "Created admin account " + normalised + ".";
}
