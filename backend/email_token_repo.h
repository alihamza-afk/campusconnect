#pragma once
// ---------------------------------------------------------------------------
// email_token_repo.h - DECLARATIONS for single-use email links.
//
// One table serves both features:
//   kind = 'verify'  -> "confirm you own this address"
//   kind = 'reset'   -> "choose a new password"
//
// Only the SHA-256 hash of the token in the link is stored, for the same reason
// sessions store a hash: a database leak must not allow someone to verify or
// reset an account.
// ---------------------------------------------------------------------------
#include <optional>
#include <string>

#include "db.h"

class EmailTokenRepo {
    Database& db_;

public:
    explicit EmailTokenRepo(Database& db);

    // Creates a token, replacing any earlier unused token of the same kind for
    // that user (so only the newest link works).
    void create(int userId, const std::string& kind, const std::string& tokenHash, long long expiresAt);

    // User id for a token that exists, has the right kind, is unused and unexpired.
    std::optional<int> findUserId(const std::string& tokenHash, const std::string& kind, long long now);

    // Marks a token used. Returns false if it was already used or has expired,
    // which makes two simultaneous clicks safe: only one can win.
    bool consume(const std::string& tokenHash, long long now);

    // True if a token of this kind was issued for the user after "since".
    // Used for the resend cooldown.
    bool recentlyIssued(int userId, const std::string& kind, long long since);

    int deleteAllForUser(int userId);
    int pruneExpired(long long now);
};
