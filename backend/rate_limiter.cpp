#include "rate_limiter.h"

#include <ctime>

namespace {
const long long KEEP_SECONDS = 24 * 60 * 60;  // prune anything untouched for a day
}

RateLimiter::RateLimiter(Database& db) : db_(db) {
    prune(static_cast<long long>(std::time(nullptr)) - KEEP_SECONDS);
}

int RateLimiter::count(const std::string& key, long long windowSeconds, long long now) {
    Statement s = db_.prepare("SELECT hits, window_start FROM rate_limits WHERE key = ?");
    s.bind(1, key);
    if (!s.step()) return 0;
    const long long windowStart = s.columnInt64(1);
    if (windowStart <= now - windowSeconds) return 0;  // window has expired
    return s.columnInt(0);
}

void RateLimiter::record(const std::string& key, long long windowSeconds, long long now) {
    // If the previous window has expired, forget it so the count restarts at 1.
    Statement expire = db_.prepare("DELETE FROM rate_limits WHERE key = ? AND window_start <= ?");
    expire.bind(1, key);
    expire.bind(2, now - windowSeconds);
    expire.run();

    // One statement, no read-then-write race: insert the first hit, otherwise ++.
    // This is SQLite's "upsert" (ON CONFLICT), available since 3.24.
    Statement hit = db_.prepare(
        "INSERT INTO rate_limits (key, hits, window_start) VALUES (?, 1, ?) "
        "ON CONFLICT(key) DO UPDATE SET hits = hits + 1");
    hit.bind(1, key);
    hit.bind(2, now);
    hit.run();
}

void RateLimiter::clear(const std::string& key) {
    Statement s = db_.prepare("DELETE FROM rate_limits WHERE key = ?");
    s.bind(1, key);
    s.run();
}

int RateLimiter::prune(long long olderThan) {
    Statement s = db_.prepare("DELETE FROM rate_limits WHERE window_start <= ?");
    s.bind(1, olderThan);
    s.run();
    return db_.changes();
}
