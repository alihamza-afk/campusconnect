#pragma once
// ---------------------------------------------------------------------------
// rate_limiter.h - fixed-window counters stored in SQLite.
//
// A key looks like "login-email:someone@example.com" or "login-ip:203.0.113.7".
// The counter is: how many hits in the current window, and when the window
// started. After the window passes the count is treated as zero.
//
// Why the database and not a map in memory?
//   * the counters survive a restart (an attacker cannot reset the lockout by
//     waiting for a crash or a deploy),
//   * one process's counters are visible to the next, and
//   * the same table serves login, registration and password-reset limits.
//
// Fixed window vs sliding window: a fixed window is the simplest correct-enough
// choice and is easy to explain. Sliding windows (or a token bucket) are more
// precise; see docs/STUDY-TOPICS.md.
// ---------------------------------------------------------------------------
#include <string>

#include "db.h"

class RateLimiter {
    Database& db_;

public:
    // Drops counters older than a day, so the table cannot grow forever.
    explicit RateLimiter(Database& db);

    // Hits in the current window, or 0 if the window has expired.
    int count(const std::string& key, long long windowSeconds, long long now);

    // Records one hit, starting a new window when the old one has expired.
    void record(const std::string& key, long long windowSeconds, long long now);

    // Forgets one key (used to forgive an account after a successful login).
    void clear(const std::string& key);

    int prune(long long olderThan);
};
