// ---------------------------------------------------------------------------
// legacy_json.cpp - one-time import of Part 1's JSON files into SQLite.
//
// Part 1 saved everything to data/items.json and data/users.json. Deleting the
// old files would throw away any existing accounts and notices, so on start-up
// we check: if the database tables are empty AND the old files exist, copy the
// rows across (keeping the same ids and password hashes), then carry on.
//
// This is the ONLY place that still reads the old JSON format. Once you are
// happy with the new database you can delete this file and the .json data files.
// ---------------------------------------------------------------------------
#include "legacy_json.h"

#include <ctime>
#include <fstream>

#include "db.h"
#include "item_repo.h"
#include "json.hpp"
#include "user_repo.h"

namespace {

// Returns false when the file does not exist (first run). Throws if it exists
// but is not valid JSON, so a corrupt file is never silently ignored.
bool readLegacyJson(const std::string& path, nlohmann::json& out) {
    std::ifstream in(path);
    if (!in) return false;
    out = nlohmann::json::parse(in);
    return true;
}

}  // namespace

std::vector<std::string> importLegacyJson(const std::string& dataDir, Database& db,
                                          UserRepo& users, ItemRepo& items) {
    std::vector<std::string> log;
    const std::string usersPath = dataDir + "/users.json";
    const std::string itemsPath = dataDir + "/items.json";
    nlohmann::json data;

    // ---- users -------------------------------------------------------------
    if (users.count() == 0 && readLegacyJson(usersPath, data)) {
        const nlohmann::json list = data.contains("users") ? data["users"] : nlohmann::json::array();
        if (list.is_array()) {
            long long imported = 0;
            Transaction tx(db);
            for (const nlohmann::json& row : list) {
                if (!row.is_object()) continue;
                // Explicit id keeps owner_id references in items.json valid.
                // INSERT OR IGNORE skips a duplicate email instead of aborting.
                Statement insert = db.prepare(
                    "INSERT OR IGNORE INTO users (id, email, name, pass_hash, role, created_at) "
                    "VALUES (?, ?, ?, ?, ?, ?)");
                insert.bind(1, row.value("id", 0))
                      .bind(2, row.value("email", std::string()))
                      .bind(3, row.value("name", std::string()))
                      .bind(4, row.value("passHash", std::string()))
                      .bind(5, row.value("role", std::string("user")))
                      .bind(6, static_cast<long long>(std::time(nullptr)));
                insert.run();
                if (db.changes() > 0) imported++;
            }
            // Accounts imported from Part 1 existed before verification
            // existed, so treat them as verified; otherwise the upgrade would
            // stop every existing user from posting.
            db.exec("UPDATE users SET email_verified_at = created_at WHERE email_verified_at IS NULL");
            tx.commit();
            log.push_back("Imported " + std::to_string(imported) + " account(s) from " + usersPath);
        }
    }

    // ---- items -------------------------------------------------------------
    if (items.loadAll().empty() && readLegacyJson(itemsPath, data)) {
        const nlohmann::json list = data.contains("items") ? data["items"] : nlohmann::json::array();
        if (list.is_array()) {
            // owner_id is a foreign key, so only import notices whose author still exists.
            std::vector<int> ownerIds;
            Statement ownerQuery = db.prepare("SELECT id FROM users");
            while (ownerQuery.step()) ownerIds.push_back(ownerQuery.columnInt(0));

            long long imported = 0, skipped = 0;
            Transaction tx(db);
            for (const nlohmann::json& row : list) {
                if (!row.is_object()) continue;
                const std::string type = row.value("type", std::string());
                if (type != "sell" && type != "lend" && type != "lost" && type != "found") {
                    skipped++;
                    continue;
                }
                const int ownerId = row.value("ownerId", 0);
                bool ownerExists = false;
                for (int id : ownerIds) ownerExists = ownerExists || (id == ownerId);
                if (!ownerExists) {
                    skipped++;
                    continue;
                }
                double price = row.contains("price") && row["price"].is_number()
                                   ? row["price"].get<double>() : 0.0;
                if (!(price >= 0 && price <= 1000000)) price = 0;  // old data may be odd
                const std::string status = row.value("status", std::string("open")) == "closed"
                                               ? "closed" : "open";

                Statement insert = db.prepare(
                    "INSERT OR IGNORE INTO items (id, owner_id, type, title, description, category, "
                    "location, price, status, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
                insert.bind(1, row.value("id", 0))
                      .bind(2, ownerId)
                      .bind(3, type)
                      .bind(4, row.value("title", std::string()))
                      .bind(5, row.value("description", std::string()))
                      .bind(6, row.value("category", std::string("Other")))
                      .bind(7, row.value("location", std::string()))
                      .bind(8, price)
                      .bind(9, status)
                      .bind(10, static_cast<long long>(row.value("createdAt", 0LL)));
                insert.run();
                if (db.changes() > 0) imported++;
                else skipped++;
            }
            tx.commit();
            std::string line = "Imported " + std::to_string(imported) + " notice(s) from " + itemsPath;
            if (skipped > 0) line += " (skipped " + std::to_string(skipped) + " row(s): unknown author or type)";
            log.push_back(line);
        }
    }
    return log;
}
