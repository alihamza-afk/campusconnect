// ---------------------------------------------------------------------------
// item_repo.cpp - the SQL for the `items` table.
//
// The notice stores only owner_id; the owner's display name is joined from
// users. That is normalisation: one fact, one place, so a name change cannot
// leave dozens of notices showing the old name.
// ---------------------------------------------------------------------------
#include "item_repo.h"

#include <ctime>

namespace {

// Column order must match the SELECT in loadAll().
Item itemFromRow(const Statement& s) {
    Item item;
    item.id = s.columnInt(0);
    item.ownerId = s.columnInt(1);
    item.type = s.columnText(2);
    item.title = s.columnText(3);
    item.description = s.columnText(4);
    item.category = s.columnText(5);
    item.location = s.columnText(6);
    item.price = s.columnDouble(7);
    item.status = s.columnText(8);
    item.createdAt = s.columnInt64(9);
    item.proof = s.columnText(10);
    item.contact = s.columnText(11);
    item.imageMime = s.columnIsNull(12) ? std::string() : s.columnText(12);
    item.owner = s.columnText(13);  // joined from users.name
    return item;
}

}  // namespace

ItemRepo::ItemRepo(Database& db) : db_(db) {}

std::vector<Item> ItemRepo::loadAll() {
    Statement s = db_.prepare(
        "SELECT i.id, i.owner_id, i.type, i.title, i.description, i.category, "
        "       i.location, i.price, i.status, i.created_at, i.proof, i.contact, "
        "       i.image_mime, u.name "
        "FROM items i LEFT JOIN users u ON u.id = i.owner_id "
        "ORDER BY i.id");
    std::vector<Item> items;
    while (s.step()) items.push_back(itemFromRow(s));
    return items;
}

Item ItemRepo::insert(Item item) {
    if (item.createdAt == 0) item.createdAt = static_cast<long long>(std::time(nullptr));
    if (item.status.empty()) item.status = "open";
    Statement s = db_.prepare(
        "INSERT INTO items (owner_id, type, title, description, category, location, price, status, "
        "                  created_at, proof, contact) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    s.bind(1, item.ownerId);
    s.bind(2, item.type);
    s.bind(3, item.title);
    s.bind(4, item.description);
    s.bind(5, item.category);
    s.bind(6, item.location);
    s.bind(7, item.price);
    s.bind(8, item.status);
    s.bind(9, item.createdAt);
    s.bind(10, item.proof);
    s.bind(11, item.contact);
    s.run();
    item.id = static_cast<int>(db_.lastInsertRowId());
    return item;
}

bool ItemRepo::setStatus(int id, const std::string& status) {
    Statement s = db_.prepare("UPDATE items SET status = ? WHERE id = ?");
    s.bind(1, status);
    s.bind(2, id);
    s.run();
    return db_.changes() > 0;
}

bool ItemRepo::remove(int id) {
    Statement s = db_.prepare("DELETE FROM items WHERE id = ?");
    s.bind(1, id);
    s.run();
    return db_.changes() > 0;
}

bool ItemRepo::setImageMime(int id, const std::string& mime) {
    Statement s = db_.prepare("UPDATE items SET image_mime = ? WHERE id = ?");
    s.bind(1, mime);
    s.bind(2, id);
    s.run();
    return db_.changes() > 0;
}

long long ItemRepo::countOpen() {
    Statement s = db_.prepare("SELECT COUNT(*) FROM items WHERE status = 'open'");
    return s.step() ? s.columnInt64(0) : 0;
}

long long ItemRepo::countClosed() {
    Statement s = db_.prepare("SELECT COUNT(*) FROM items WHERE status = 'closed'");
    return s.step() ? s.columnInt64(0) : 0;
}

long long ItemRepo::countByType(const std::string& type) {
    Statement s = db_.prepare("SELECT COUNT(*) FROM items WHERE type = ?");
    s.bind(1, type);
    return s.step() ? s.columnInt64(0) : 0;
}
