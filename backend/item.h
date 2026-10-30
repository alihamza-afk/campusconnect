#pragma once
#include <string>

struct Item {
    int id = 0;
    std::string type;
    std::string title;
    std::string description;
    std::string category;
    std::string location;
    std::string owner;
    double price = 0;
    long long createdAt = 0;
    std::string status = "open";
};
