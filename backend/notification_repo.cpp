#include "notification_repo.h"
#include <ctime>

NotificationRepo::NotificationRepo(Database& db) : db_(db) {}

void NotificationRepo::create(int userId, const std::string& kind, int itemId, const std::string& text) {
    Statement s = db_.prepare(
        "INSERT INTO notifications (user_id, kind, item_id, text, created_at) VALUES (?, ?, ?, ?, ?)");
    s.bind(1, userId);
    s.bind(2, kind);
    s.bind(3, itemId);
    s.bind(4, text);
    s.bind(5, static_cast<long long>(std::time(nullptr)));
    s.run();
}

std::vector<Notification> NotificationRepo::listFor(int userId, int limit) {
    Statement s = db_.prepare(
        "SELECT id, kind, item_id, text, created_at, read_at FROM notifications "
        "WHERE user_id = ? ORDER BY created_at DESC, id DESC LIMIT ?");
    s.bind(1, userId);
    s.bind(2, limit);
    std::vector<Notification> out;
    while (s.step()) {
        Notification n;
        n.id = s.columnInt(0);
        n.kind = s.columnText(1);
        n.itemId = s.columnInt(2);
        n.text = s.columnText(3);
        n.createdAt = s.columnInt64(4);
        n.read = !s.columnIsNull(5);
        out.push_back(n);
    }
    return out;
}

int NotificationRepo::unreadCount(int userId) {
    Statement s = db_.prepare("SELECT COUNT(*) FROM notifications WHERE user_id = ? AND read_at IS NULL");
    s.bind(1, userId);
    return s.step() ? s.columnInt(0) : 0;
}

void NotificationRepo::markAllRead(int userId, long long when) {
    Statement s = db_.prepare("UPDATE notifications SET read_at = ? WHERE user_id = ? AND read_at IS NULL");
    s.bind(1, when);
    s.bind(2, userId);
    s.run();
}
