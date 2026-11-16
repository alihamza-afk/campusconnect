#include "purchase_request_repo.h"
#include <ctime>

PurchaseRequestRepo::PurchaseRequestRepo(Database& db) : db_(db) {}

int PurchaseRequestRepo::create(
    int itemId, int buyerId, double offeredPrice,
    const std::string& message, const std::string& buyerContact) {

    Statement s = db_.prepare(
        "INSERT OR IGNORE INTO purchase_requests "
        "(item_id, buyer_id, offered_price, message, buyer_contact, status, created_at) "
        "SELECT ?, ?, ?, ?, ?, 'pending', ? "
        "WHERE EXISTS ("
        "SELECT 1 FROM items "
        "WHERE id = ? AND type = 'sell' AND status = 'open' AND owner_id <> ?"
        ")");

    s.bind(1, itemId);
    s.bind(2, buyerId);
    s.bind(3, offeredPrice);
    s.bind(4, message);
    s.bind(5, buyerContact);
    s.bind(6, static_cast<long long>(std::time(nullptr)));
    s.bind(7, itemId);
    s.bind(8, buyerId);
    s.run();

    if (db_.changes() == 0) return -1;

    return static_cast<int>(db_.lastInsertRowId());
}

std::vector<PurchaseRequest> PurchaseRequestRepo::listForSeller(int itemId) {
    std::vector<PurchaseRequest> requests;

    Statement s = db_.prepare(
        "SELECT pr.id, pr.item_id, pr.buyer_id, u.name, i.title, "
        "pr.offered_price, pr.message, pr.buyer_contact, i.contact, "
        "pr.status, pr.created_at, COALESCE(pr.decided_at, 0) "
        "FROM purchase_requests pr "
        "JOIN users u ON u.id = pr.buyer_id "
        "JOIN items i ON i.id = pr.item_id "
        "WHERE pr.item_id = ? ORDER BY pr.created_at DESC");

    s.bind(1, itemId);

    while (s.step()) {
        PurchaseRequest r;
        r.id = s.columnInt(0);
        r.itemId = s.columnInt(1);
        r.buyerId = s.columnInt(2);
        r.buyerName = s.columnText(3);
        r.itemTitle = s.columnText(4);
        r.offeredPrice = s.columnDouble(5);
        r.message = s.columnText(6);
        r.buyerContact = s.columnText(7);
        r.sellerContact = s.columnText(8);
        r.status = s.columnText(9);
        r.createdAt = s.columnInt64(10);
        r.decidedAt = s.columnInt64(11);
        requests.push_back(r);
    }

    return requests;
}

std::vector<PurchaseRequest> PurchaseRequestRepo::listForBuyer(int buyerId) {
    std::vector<PurchaseRequest> requests;

    Statement s = db_.prepare(
        "SELECT pr.id, pr.item_id, pr.buyer_id, u.name, i.title, "
        "pr.offered_price, pr.message, pr.buyer_contact, i.contact, "
        "pr.status, pr.created_at, COALESCE(pr.decided_at, 0) "
        "FROM purchase_requests pr "
        "JOIN users u ON u.id = pr.buyer_id "
        "JOIN items i ON i.id = pr.item_id "
        "WHERE pr.buyer_id = ? ORDER BY pr.created_at DESC");

    s.bind(1, buyerId);

    while (s.step()) {
        PurchaseRequest r;
        r.id = s.columnInt(0);
        r.itemId = s.columnInt(1);
        r.buyerId = s.columnInt(2);
        r.buyerName = s.columnText(3);
        r.itemTitle = s.columnText(4);
        r.offeredPrice = s.columnDouble(5);
        r.message = s.columnText(6);
        r.buyerContact = s.columnText(7);
        r.sellerContact = s.columnText(8);
        r.status = s.columnText(9);
        r.createdAt = s.columnInt64(10);
        r.decidedAt = s.columnInt64(11);
        requests.push_back(r);
    }

    return requests;
}

PurchaseDecisionResult PurchaseRequestRepo::decide(
    int itemId, int requestId, int sellerId, bool accept, long long now) {

    Transaction tx(db_);

    Statement update = db_.prepare(
        "UPDATE purchase_requests SET status = ?, decided_at = ?, decided_by = ? "
        "WHERE id = ? AND item_id = ? AND status = 'pending' "
        "AND EXISTS (SELECT 1 FROM items WHERE id = ? AND owner_id = ? AND status = 'open')");

    update.bind(1, accept ? "accepted" : "rejected");
    update.bind(2, now);
    update.bind(3, sellerId);
    update.bind(4, requestId);
    update.bind(5, itemId);
    update.bind(6, itemId);
    update.bind(7, sellerId);
    update.run();

    if (db_.changes() == 0) {
        return {PurchaseDecisionResult::Status::NotFound, {}};
    }

    std::vector<int> rejectedIds;

    if (accept) {
        Statement others = db_.prepare(
            "SELECT buyer_id FROM purchase_requests "
            "WHERE item_id = ? AND id <> ? AND status = 'pending'");

        others.bind(1, itemId);
        others.bind(2, requestId);

        while (others.step()) {
            rejectedIds.push_back(others.columnInt(0));
        }

        Statement rejectOthers = db_.prepare(
            "UPDATE purchase_requests SET status = 'rejected', "
            "decided_at = ?, decided_by = ? "
            "WHERE item_id = ? AND id <> ? AND status = 'pending'");

        rejectOthers.bind(1, now);
        rejectOthers.bind(2, sellerId);
        rejectOthers.bind(3, itemId);
        rejectOthers.bind(4, requestId);
        rejectOthers.run();

        Statement closeItem = db_.prepare(
            "UPDATE items SET status = 'closed' WHERE id = ? AND owner_id = ?");

        closeItem.bind(1, itemId);
        closeItem.bind(2, sellerId);
        closeItem.run();
            if (db_.changes() != 1) {
            return {PurchaseDecisionResult::Status::NotFound, {}};
        }
}

    tx.commit();

    return {PurchaseDecisionResult::Status::Ok, rejectedIds};
}
