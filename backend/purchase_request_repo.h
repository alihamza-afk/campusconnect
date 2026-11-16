#pragma once

#include <string>
#include <vector>
#include "db.h"

struct PurchaseRequest {
    int id = 0;
    int itemId = 0;
    int buyerId = 0;
    std::string buyerName;
    std::string itemTitle;
    double offeredPrice = 0;
    std::string message;
    std::string buyerContact;
    std::string sellerContact;
    std::string status = "pending";
    long long createdAt = 0;
    long long decidedAt = 0;
};

struct PurchaseDecisionResult {
    enum class Status { Ok, NotFound };
    Status status;
    std::vector<int> rejectedBuyerIds;
};

class PurchaseRequestRepo {
    Database& db_;

public:
    explicit PurchaseRequestRepo(Database& db);

    int create(int itemId, int buyerId, double offeredPrice,
               const std::string& message, const std::string& buyerContact);

    std::vector<PurchaseRequest> listForSeller(int itemId);
    std::vector<PurchaseRequest> listForBuyer(int buyerId);

    PurchaseDecisionResult decide(int itemId, int requestId,
                                  int sellerId, bool accept, long long now);
};
