#pragma once
#include <string>
#include <vector>
#include "db.h"

struct Notification {
    int id = 0;
    std::string kind;
    int itemId = 0;
    std::string text;
    long long createdAt = 0;
    bool read = false;
};

class NotificationRepo {
    Database& db_;
public:
    explicit NotificationRepo(Database& db);
    void create(int userId, const std::string& kind, int itemId, const std::string& text);
    std::vector<Notification> listFor(int userId, int limit);
    int unreadCount(int userId);
    void markAllRead(int userId, long long when);
};
