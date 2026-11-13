#pragma once
// ---------------------------------------------------------------------------
// session_repo.h - DECLARATIONS for login sessions in the database.
//
// A session row is: which user, when it started, when it expires, and the
// SHA-256 hash of the random token that the browser holds in its cookie.
//
// Why store only the hash?
//   * A database backup, a dumped file, or read access to the disk would
//     otherwise be enough to impersonate every logged-in user.
//   * The hash is not reversible, and tokens are 256 bits of randomness, so
//     guessing or brute-forcing one is not realistic (unlike a password, which
//     is why passwords use slow Argon2id hashing while tokens use fast SHA-256).
// ---------------------------------------------------------------------------
#include <optional>
#include <string>

#include "db.h"

class SessionRepo {
    Database& db_;

public:
    explicit SessionRepo(Database& db);

    // Inserts a session. Also trims older sessions for that user so one account
    // cannot accumulate thousands of rows (MAX_SESSIONS_PER_USER in session_repo.cpp).
    void create(int userId, const std::string& tokenHash, long long expiresAt);

    // Returns the user id if the session exists AND has not expired.
    std::optional<int> findUserId(const std::string& tokenHash, long long now);

    // One session (normal "log out").
    bool remove(const std::string& tokenHash);

    // Every session for one user ("log out everywhere"). Returns how many ended.
    int removeAllForUser(int userId);

    // Deletes rows whose expiry has passed. Called at start-up and on login.
    int pruneExpired(long long now);

    // Used by tests and (later) by a "your devices" screen.
    int countForUser(int userId);

    // Phase 8: total live (non-expired) sessions across all users.
    long long countActive(long long now);
};
