#pragma once
// ---------------------------------------------------------------------------
// password.h - Argon2id password hashing, in one place.
//
// The hashing used to live inside auth.cpp. Email verification and password
// reset also need it (a reset stores a new password hash), so it is now a
// small module of its own that Auth and EmailService can both call.
// ---------------------------------------------------------------------------
#include <string>

// Argon2id hash string. It contains the random salt and the cost settings, so
// only this one string needs to be stored. Throws std::runtime_error if the
// machine is out of memory.
std::string hashPassword(const std::string& password);

bool verifyPassword(const std::string& hash, const std::string& password);
