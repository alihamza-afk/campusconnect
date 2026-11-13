#pragma once
#include <string>

struct Item {
    int id = 0;
    std::string type;
    std::string title;
    std::string description;
    std::string category;
    std::string location;
    std::string owner;      // display name, copied from the poster's account
    int ownerId = 0;        // id of the account that posted it; used for permission checks
    double price = 0;
    long long createdAt = 0;
    std::string status = "open";
    // Phase 6: private fields for lost & found claims. They must never be
    // sent to anyone except the owner (contact is revealed to a claimant
    // only after the owner approves the claim).
    std::string proof;    // a detail only the real owner would know
    std::string contact;  // how the owner can be reached
    // Phase 8: optional attached photo. MIME type stored in the database;
    // the bytes live on disk as data/images/<id><ext>.
    std::string imageMime;  // e.g. "image/jpeg", or "" when there is no image
};
