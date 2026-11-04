#pragma once
// Saving and loading JSON files on disk, so items and accounts survive a restart.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "json.hpp"
#include "item.h"

using json = nlohmann::json;

inline json itemToJson(const Item& it) {
    return json{
        {"id", it.id}, {"type", it.type}, {"title", it.title},
        {"description", it.description}, {"category", it.category},
        {"location", it.location}, {"owner", it.owner}, {"ownerId", it.ownerId},
        {"price", it.price}, {"createdAt", it.createdAt}, {"status", it.status}
    };
}

inline Item itemFromJson(const json& j) {
    Item it;
    it.id = j.value("id", 0);
    it.type = j.value("type", "");
    it.title = j.value("title", "");
    it.description = j.value("description", "");
    it.category = j.value("category", "Other");
    it.location = j.value("location", "");
    it.owner = j.value("owner", "");
    it.ownerId = j.value("ownerId", 0);
    it.price = j.value("price", 0.0);
    it.createdAt = j.value("createdAt", 0LL);
    it.status = j.value("status", "open");
    return it;
}

// Returns false if the file does not exist yet (first run).
// Throws if it exists but is not valid JSON, so a corrupt file is never silently replaced.
inline bool readJsonFile(const std::string& path, json& out) {
    std::ifstream in(path);
    if (!in) return false;
    out = json::parse(in);
    return true;
}

// Write to a temporary file, then rename it over the real one. rename() replaces the file
// in one step, so a crash halfway through leaves the old file intact instead of a half-written one.
// ownerOnly restricts the file to the server's own OS user (for files holding password hashes).
inline void writeJsonFileAtomic(const std::string& path, const json& data, bool ownerOnly = false) {
    namespace fs = std::filesystem;
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp);
        if (ownerOnly) fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
        out << data.dump(2);
        out.flush();
        if (!out) throw std::runtime_error("write failed for " + tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error("cannot replace " + path);
}
