#include "secrets.h"

#include <sodium.h>

std::string randomToken() {
    unsigned char raw[32];
    randombytes_buf(raw, sizeof raw);
    char hex[sizeof raw * 2 + 1];
    sodium_bin2hex(hex, sizeof hex, raw, sizeof raw);
    return hex;
}

std::string hashToken(const std::string& token) {
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, reinterpret_cast<const unsigned char*>(token.data()), token.size());
    char hex[sizeof digest * 2 + 1];
    sodium_bin2hex(hex, sizeof hex, digest, sizeof digest);
    return hex;
}
