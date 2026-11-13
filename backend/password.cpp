#include "password.h"

#include <sodium.h>

#include <stdexcept>

std::string hashPassword(const std::string& password) {
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(out, password.c_str(), password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("out of memory while hashing password");
    }
    return out;
}

bool verifyPassword(const std::string& hash, const std::string& password) {
    return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
}
