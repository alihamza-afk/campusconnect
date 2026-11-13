#pragma once
// ---------------------------------------------------------------------------
// auth.h - DECLARATIONS for account rules, login and sessions.
//
// Phase 4 change: the failed-login counters no longer live in a std::map in
// memory. They are rate-limit rows in SQLite (rate_limiter.h), keyed by both
// the email address and the client IP. That means:
//   * a restart cannot clear a lockout,
//   * one network cannot spray thousands of guesses across many accounts,
//   * the same counter table also limits sign-ups and reset requests.
//
// Roles live on the server. Nobody can make themselves admin from the browser.
// ---------------------------------------------------------------------------
#include <mutex>
#include <string>

#include "rate_limiter.h"
#include "session_repo.h"
#include "user_repo.h"

class Auth {
public:
    enum class LoginResult { Ok, Bad, Locked };

    static constexpr long long SESSION_SECONDS = 24 * 60 * 60;
    static constexpr int MAX_FAILURES = 5;              // failed logins allowed per email...
    static constexpr long long LOCK_SECONDS = 15 * 60;  // ...and per IP, within this window

    Auth(UserRepo& users, SessionRepo& sessions, RateLimiter& rateLimiter);

    void setAllowedDomain(const std::string& domain);

    // Lower-case + trim. Public so /api/forgot normalises the same way.
    static std::string normalizeEmail(const std::string& email);

    // Returns an error message, or "" on success (and fills *out).
    std::string registerUser(const std::string& emailIn, const std::string& nameIn,
                             const std::string& password, const std::string& role,
                             bool enforceDomain, User* out);

    // clientIp is "" when it is not known (some tests); the IP counter is then skipped.
    LoginResult login(const std::string& emailIn, const std::string& password,
                      const std::string& clientIp, std::string& tokenOut, User& userOut);

    // Convenience overload for callers that do not have an address (unit tests).
    LoginResult login(const std::string& emailIn, const std::string& password,
                      std::string& tokenOut, User& userOut) {
        return login(emailIn, password, std::string(), tokenOut, userOut);
    }

    // Starts a session for a user who just registered.
    std::string startSession(int userId);

    bool userForToken(const std::string& token, User& out);
    void logout(const std::string& token);
    // Ends every session for this user ("log out everywhere"). Returns how many.
    int logoutAll(int userId);

    // Creates the first admin from server configuration. Does nothing if an
    // admin already exists. Returns a message for the server log.
    std::string ensureAdmin(const std::string& email, const std::string& name,
                            const std::string& password);

private:
    UserRepo& users_;
    SessionRepo& sessions_;
    RateLimiter& rateLimiter_;
    std::string allowedDomain;   // e.g. "geu.ac.in"; empty = any domain
    std::string dummyHash;       // verified for unknown emails, so timing does not reveal which emails exist
    std::mutex mtx;              // guards allowedDomain
};
