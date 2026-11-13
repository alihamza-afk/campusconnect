// ---------------------------------------------------------------------------
// user_repo.cpp - the SQL for the `users` table.
//
// Every value that comes from outside this file is bound with ?, never pasted
// into the SQL text. That is the whole SQL-injection defence.
// ---------------------------------------------------------------------------
#include "user_repo.h"

#include <ctime>

namespace {

// Column order must match the SELECT in this file.
User userFromRow(const Statement& s) {
    User user;
    user.id = s.columnInt(0);
    user.email = s.columnText(1);
    user.name = s.columnText(2);
    user.passHash = s.columnText(3);
    user.role = s.columnText(4);
    user.createdAt = s.columnInt64(5);
    user.emailVerifiedAt = s.columnIsNull(6) ? 0 : s.columnInt64(6);
    user.bannedAt = s.columnIsNull(7) ? 0 : s.columnInt64(7);
    return user;
}

const char* SELECT_COLUMNS =
    "id, email, name, pass_hash, role, created_at, email_verified_at, banned_at";

}  // namespace

UserRepo::UserRepo(Database& db) : db_(db) {}

std::optional<User> UserRepo::findById(int id) {
    Statement s = db_.prepare(std::string("SELECT ") + SELECT_COLUMNS + " FROM users WHERE id = ?");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return userFromRow(s);
}

std::optional<User> UserRepo::findByEmail(const std::string& email) {
    Statement s = db_.prepare(std::string("SELECT ") + SELECT_COLUMNS + " FROM users WHERE email = ?");
    s.bind(1, email);
    if (!s.step()) return std::nullopt;
    return userFromRow(s);
}

bool UserRepo::emailExists(const std::string& email) {
    Statement s = db_.prepare("SELECT 1 FROM users WHERE email = ? LIMIT 1");
    s.bind(1, email);
    return s.step();
}

long long UserRepo::count() {
    Statement s = db_.prepare("SELECT COUNT(*) FROM users");
    return s.step() ? s.columnInt64(0) : 0;
}

int UserRepo::countAdmins() {
    Statement s = db_.prepare("SELECT COUNT(*) FROM users WHERE role = 'admin'");
    return s.step() ? s.columnInt(0) : 0;
}

long long UserRepo::countSince(long long sinceEpoch) {
    Statement s = db_.prepare("SELECT COUNT(*) FROM users WHERE created_at >= ?");
    s.bind(1, sinceEpoch);
    return s.step() ? s.columnInt64(0) : 0;
}

void UserRepo::insert(User& user) {
    if (user.createdAt == 0) user.createdAt = static_cast<long long>(std::time(nullptr));
    // The UNIQUE constraint on email is the real guarantee that two requests
    // cannot both create the same account; the caller's check is only a nicer
    // error message.
    Statement s = db_.prepare(
        "INSERT INTO users (email, name, pass_hash, role, created_at) VALUES (?, ?, ?, ?, ?)");
    s.bind(1, user.email);
    s.bind(2, user.name);
    s.bind(3, user.passHash);
    s.bind(4, user.role);
    s.bind(5, user.createdAt);
    s.run();
    user.id = static_cast<int>(db_.lastInsertRowId());
}

void UserRepo::promoteToAdmin(int id) {
    Statement s = db_.prepare("UPDATE users SET role = 'admin' WHERE id = ?");
    s.bind(1, id);
    s.run();
}

void UserRepo::markVerified(int id, long long when) {
    Statement s = db_.prepare("UPDATE users SET email_verified_at = ? WHERE id = ?");
    s.bind(1, when);
    s.bind(2, id);
    s.run();
}

void UserRepo::setPasswordHash(int id, const std::string& passHash) {
    Statement s = db_.prepare("UPDATE users SET pass_hash = ? WHERE id = ?");
    s.bind(1, passHash);
    s.bind(2, id);
    s.run();
}

void UserRepo::ban(int id, long long when) {
    Statement s = db_.prepare("UPDATE users SET banned_at = ? WHERE id = ?");
    s.bind(1, when);
    s.bind(2, id);
    s.run();
}

void UserRepo::unban(int id) {
    Statement s = db_.prepare("UPDATE users SET banned_at = NULL WHERE id = ?");
    s.bind(1, id);
    s.run();
}

std::vector<User> UserRepo::adminList() {
    Statement s = db_.prepare(std::string("SELECT ") + SELECT_COLUMNS +
                              " FROM users ORDER BY created_at DESC, id DESC");
    std::vector<User> users;
    while (s.step()) users.push_back(userFromRow(s));
    return users;
}
