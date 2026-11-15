#include "email_token_repo.h"
#include <ctime>

EmailTokenRepo::EmailTokenRepo(Database& db) : db_(db) {}

void EmailTokenRepo::create(int userId, const std::string& kind, const std::string& tokenHash,
                            long long expiresAt) {
    // Ask for a new link twice? The first one stops working. That keeps the
    // table small and avoids two valid reset links floating around.
    Statement clear = db_.prepare(
        "DELETE FROM email_tokens WHERE user_id = ? AND kind = ? AND consumed_at IS NULL");
    clear.bind(1, userId);
    clear.bind(2, kind);
    clear.run();

    Statement insert = db_.prepare(
        "INSERT INTO email_tokens (token_hash, user_id, kind, created_at, expires_at) "
        "VALUES (?, ?, ?, ?, ?)");
    insert.bind(1, tokenHash);
    insert.bind(2, userId);
    insert.bind(3, kind);
    insert.bind(4, static_cast<long long>(std::time(nullptr)));
    insert.bind(5, expiresAt);
    insert.run();
}

std::optional<int> EmailTokenRepo::findUserId(const std::string& tokenHash, const std::string& kind,
                                              long long now) {
    Statement s = db_.prepare(
        "SELECT user_id FROM email_tokens "
        "WHERE token_hash = ? AND kind = ? AND consumed_at IS NULL AND expires_at > ?");
    s.bind(1, tokenHash);
    s.bind(2, kind);
    s.bind(3, now);
    if (!s.step()) return std::nullopt;
    return s.columnInt(0);
}

bool EmailTokenRepo::consume(const std::string& tokenHash, long long now) {
    // The conditions are in the UPDATE itself, so two requests cannot both win:
    // the second one finds consumed_at already set and changes zero rows.
    Statement s = db_.prepare(
        "UPDATE email_tokens SET consumed_at = ? "
        "WHERE token_hash = ? AND consumed_at IS NULL AND expires_at > ?");
    s.bind(1, now);
    s.bind(2, tokenHash);
    s.bind(3, now);
    s.run();
    return db_.changes() > 0;
}

bool EmailTokenRepo::recentlyIssued(int userId, const std::string& kind, long long since) {
    Statement s = db_.prepare(
        "SELECT 1 FROM email_tokens WHERE user_id = ? AND kind = ? AND created_at > ? LIMIT 1");
    s.bind(1, userId);
    s.bind(2, kind);
    s.bind(3, since);
    return s.step();
}

int EmailTokenRepo::deleteToken(const std::string& tokenHash) {
    Statement s = db_.prepare("DELETE FROM email_tokens WHERE token_hash = ?");
    s.bind(1, tokenHash);
    s.run();
    return db_.changes();
}

int EmailTokenRepo::deleteAllForUser(int userId) {
    Statement s = db_.prepare("DELETE FROM email_tokens WHERE user_id = ?");
    s.bind(1, userId);
    s.run();
    return db_.changes();
}

int EmailTokenRepo::pruneExpired(long long now) {
    Statement s = db_.prepare("DELETE FROM email_tokens WHERE expires_at <= ?");
    s.bind(1, now);
    s.run();
    return db_.changes();
}
