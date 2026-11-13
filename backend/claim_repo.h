#pragma once
// ---------------------------------------------------------------------------
// claim_repo.h - DECLARATIONS for lost & found claim verification.
//
// A claim is one person saying "this lost/found notice is about my item",
// with an answer to the private detail the owner wrote, and a way to contact
// them. The owner (or an admin) approves or rejects each claim.
//
// The central rule: the owner's contact is revealed to the claimant only after
// approval, and the claimant's contact is visible only to the owner. Neither is
// ever part of the public notice JSON.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

#include "db.h"

struct Claim {
    int id = 0;
    int itemId = 0;
    int claimantId = 0;
    std::string claimantName;  // joined from users
    std::string answer;
    std::string message;
    std::string contact;       // how the claimant can be reached
    std::string status = "pending";
    long long createdAt = 0;
    long long decidedAt = 0;
};

// What a claimant sees in "my claims".
struct MyClaim {
    int id = 0;
    int itemId = 0;
    std::string itemTitle;
    std::string itemType;
    std::string status = "pending";
    std::string ownerName;
    std::string ownerContact;  // empty unless the claim was approved
    long long createdAt = 0;
};

// Extended claim row for the admin dashboard (includes item title).
struct AdminClaim {
    int id = 0;
    int itemId = 0;
    std::string itemTitle;
    int claimantId = 0;
    std::string claimantName;
    std::string status;
    long long createdAt = 0;
};

// Returned by decide() when a claim is approved, so the caller can notify losers.
struct DecideResult {
    enum class Status { Ok, NotFound } status;
    std::vector<int> rejectedClaimantIds;  // claimant ids whose claims were auto-rejected
};

class ClaimRepo {
    Database& db_;

public:
    explicit ClaimRepo(Database& db);

    bool exists(int itemId, int claimantId);
    int countPending(int itemId);

    // Returns the new claim id, or -1 when this user already claimed the item
    // (the UNIQUE constraint decides, so two simultaneous requests cannot both win).
    int create(int itemId, int claimantId, const std::string& answer,
               const std::string& message, const std::string& contact);

    std::vector<Claim> listForItem(int itemId);
    std::vector<MyClaim> listForClaimant(int claimantId);

    // Phase 8: list all claims across all items (for the admin dashboard).
    std::vector<AdminClaim> listAll(int limit, int offset);
    long long countByStatus(const std::string& status);

    // Approves or rejects one claim. On approval, the item is closed and every
    // other pending claim is rejected in the same transaction.
    // Returns the ids of claimants whose pending claims were auto-rejected (for notifications).
    DecideResult decide(int itemId, int claimId, int deciderId, bool approve, long long now);
};
