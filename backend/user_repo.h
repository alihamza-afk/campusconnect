#pragma once
// ---------------------------------------------------------------------------
// user_repo.h - DECLARATIONS for the SQL that touches the `users` table.
//
// "Repository" is a name for the class that owns the SQL for one table. Keeping
// SQL in one place per table means no query strings scattered through the web
// handlers, and every query is a prepared statement.
//
// This class does not decide policy (is the password strong? is the email
// allowed?). That is Auth's job. This class only reads and writes rows.
// ---------------------------------------------------------------------------
#include <optional>
#include <string>
#include <vector>

#include "db.h"

// The account record as the rest of the program sees it. The password is the
// Argon2id hash, never the plain password.
struct User {
    int id = 0;
    std::string email;      // always lower-case (normalised before saving)
    std::string name;
    std::string passHash;   // Argon2id string, includes salt + cost settings
    std::string role = "user";
    long long createdAt = 0;
    long long emailVerifiedAt = 0;   // 0 = not verified yet
    long long bannedAt = 0;          // 0 = not banned (Phase 8)
};

class UserRepo {
    Database& db_;

public:
    explicit UserRepo(Database& db);

    std::optional<User> findById(int id);
    std::optional<User> findByEmail(const std::string& email);
    bool emailExists(const std::string& email);
    long long count();
    int countAdmins();

    // Phase 8: stats helpers.
    long long countSince(long long sinceEpoch);   // accounts created after this timestamp

    // Inserts the user and fills in id + createdAt.
    void insert(User& user);
    void promoteToAdmin(int id);
    void markVerified(int id, long long when);
    void setPasswordHash(int id, const std::string& passHash);

    // Phase 8: banning. A banned user is refused login and their existing
    // sessions are rejected by Auth::userForToken.
    void ban(int id, long long when);
    void unban(int id);

    // Phase 8: list all users for the admin dashboard.
    std::vector<User> adminList();
};
