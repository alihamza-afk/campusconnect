#include "claim_repo.h"
#include <ctime>

ClaimRepo::ClaimRepo(Database& db) : db_(db) {}

bool ClaimRepo::exists(int itemId, int claimantId) {
    Statement s = db_.prepare("SELECT 1 FROM claims WHERE item_id = ? AND claimant_id = ? LIMIT 1");
    s.bind(1, itemId);
    s.bind(2, claimantId);
    return s.step();
}

int ClaimRepo::countPending(int itemId) {
    Statement s = db_.prepare("SELECT COUNT(*) FROM claims WHERE item_id = ? AND status = 'pending'");
    s.bind(1, itemId);
    return s.step() ? s.columnInt(0) : 0;
}

int ClaimRepo::create(int itemId, int claimantId, const std::string& answer,
                      const std::string& message, const std::string& contact) {
    // INSERT OR IGNORE + changes(): the UNIQUE(item_id, claimant_id) constraint
    // is what actually prevents a double claim, not the exists() check above.
    Statement s = db_.prepare(
        "INSERT OR IGNORE INTO claims (item_id, claimant_id, answer, message, contact, status, created_at) "
        "VALUES (?, ?, ?, ?, ?, 'pending', ?)");
    s.bind(1, itemId);
    s.bind(2, claimantId);
    s.bind(3, answer);
    s.bind(4, message);
    s.bind(5, contact);
    s.bind(6, static_cast<long long>(std::time(nullptr)));
    s.run();
    if (db_.changes() == 0) return -1;
    return static_cast<int>(db_.lastInsertRowId());
}

std::vector<Claim> ClaimRepo::listForItem(int itemId) {
    Statement s = db_.prepare(
        "SELECT c.id, c.item_id, c.claimant_id, u.name, c.answer, c.message, c.contact, "
        "       c.status, c.created_at, c.decided_at "
        "FROM claims c JOIN users u ON u.id = c.claimant_id "
        "WHERE c.item_id = ? ORDER BY c.created_at ASC, c.id ASC");
    s.bind(1, itemId);
    std::vector<Claim> claims;
    while (s.step()) {
        Claim claim;
        claim.id = s.columnInt(0);
        claim.itemId = s.columnInt(1);
        claim.claimantId = s.columnInt(2);
        claim.claimantName = s.columnText(3);
        claim.answer = s.columnText(4);
        claim.message = s.columnText(5);
        claim.contact = s.columnText(6);
        claim.status = s.columnText(7);
        claim.createdAt = s.columnInt64(8);
        claim.decidedAt = s.columnInt64(9);
        claims.push_back(claim);
    }
    return claims;
}

std::vector<MyClaim> ClaimRepo::listForClaimant(int claimantId) {
    Statement s = db_.prepare(
        "SELECT c.id, c.item_id, i.title, i.type, c.status, u.name, "
        "       CASE WHEN c.status = 'approved' THEN i.contact ELSE NULL END, "
        "       c.created_at "
        "FROM claims c "
        "JOIN items i ON i.id = c.item_id "
        "JOIN users u ON u.id = i.owner_id "
        "WHERE c.claimant_id = ? ORDER BY c.created_at DESC, c.id DESC");
    s.bind(1, claimantId);
    std::vector<MyClaim> claims;
    while (s.step()) {
        MyClaim claim;
        claim.id = s.columnInt(0);
        claim.itemId = s.columnInt(1);
        claim.itemTitle = s.columnText(2);
        claim.itemType = s.columnText(3);
        claim.status = s.columnText(4);
        claim.ownerName = s.columnText(5);
        claim.ownerContact = s.columnText(6);  // NULL -> "" (only set when approved)
        claim.createdAt = s.columnInt64(7);
        claims.push_back(claim);
    }
    return claims;
}

std::vector<AdminClaim> ClaimRepo::listAll(int limit, int offset) {
    Statement s = db_.prepare(
        "SELECT c.id, c.item_id, i.title, c.claimant_id, u.name, c.status, c.created_at "
        "FROM claims c "
        "JOIN items i ON i.id = c.item_id "
        "JOIN users u ON u.id = c.claimant_id "
        "ORDER BY c.created_at DESC, c.id DESC "
        "LIMIT ? OFFSET ?");
    s.bind(1, limit);
    s.bind(2, offset);
    std::vector<AdminClaim> claims;
    while (s.step()) {
        AdminClaim ac;
        ac.id = s.columnInt(0);
        ac.itemId = s.columnInt(1);
        ac.itemTitle = s.columnText(2);
        ac.claimantId = s.columnInt(3);
        ac.claimantName = s.columnText(4);
        ac.status = s.columnText(5);
        ac.createdAt = s.columnInt64(6);
        claims.push_back(ac);
    }
    return claims;
}

long long ClaimRepo::countByStatus(const std::string& claimStatus) {
    Statement s = db_.prepare("SELECT COUNT(*) FROM claims WHERE status = ?");
    s.bind(1, claimStatus);
    return s.step() ? s.columnInt64(0) : 0;
}

DecideResult ClaimRepo::decide(int itemId, int claimId, int deciderId, bool approve, long long now) {
    Transaction tx(db_);

    // The status check is inside the UPDATE, so two people clicking "approve"
    // at the same time cannot both win.
    Statement update = db_.prepare(
        "UPDATE claims SET status = ?, decided_at = ?, decided_by = ? "
        "WHERE id = ? AND item_id = ? AND status = 'pending'");
    update.bind(1, approve ? "approved" : "rejected");
    update.bind(2, now);
    update.bind(3, deciderId);
    update.bind(4, claimId);
    update.bind(5, itemId);
    update.run();
    if (db_.changes() == 0) return {DecideResult::Status::NotFound, {}};

    std::vector<int> rejectedIds;
    if (approve) {
        // The item is now resolved, and every other pending claim loses.
        Statement closeItem = db_.prepare("UPDATE items SET status = 'closed' WHERE id = ?");
        closeItem.bind(1, itemId);
        closeItem.run();

        // Collect the claimant ids before rejecting, so callers can send notifications.
        Statement others = db_.prepare(
            "SELECT claimant_id FROM claims WHERE item_id = ? AND id <> ? AND status = 'pending'");
        others.bind(1, itemId);
        others.bind(2, claimId);
        while (others.step()) rejectedIds.push_back(others.columnInt(0));

        Statement rejectOthers = db_.prepare(
            "UPDATE claims SET status = 'rejected', decided_at = ?, decided_by = ? "
            "WHERE item_id = ? AND id <> ? AND status = 'pending'");
        rejectOthers.bind(1, now);
        rejectOthers.bind(2, deciderId);
        rejectOthers.bind(3, itemId);
        rejectOthers.bind(4, claimId);
        rejectOthers.run();
    }

    tx.commit();
    return {DecideResult::Status::Ok, rejectedIds};
}
