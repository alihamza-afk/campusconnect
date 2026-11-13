#pragma once
// ---------------------------------------------------------------------------
// secrets.h - random secrets and their hashes.
//
// Used for login sessions (Phase 2) and for email verification / password
// reset links (Phase 3). Both are the same kind of thing: a random token given
// to one person, and a SHA-256 fingerprint stored in the database.
//
// Why SHA-256 and not Argon2id here? Because the token is already 256 random
// bits - there is nothing to guess. Passwords need Argon2id (slow, salted)
// because humans choose weak ones. See password.h.
// ---------------------------------------------------------------------------
#include <string>

// 32 random bytes from libsodium, hex-encoded (64 characters).
std::string randomToken();

// SHA-256 hex digest (64 characters). What actually goes into the database.
std::string hashToken(const std::string& token);
