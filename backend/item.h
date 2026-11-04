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
};
