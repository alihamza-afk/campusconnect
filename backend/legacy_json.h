#pragma once
// ---------------------------------------------------------------------------
// legacy_json.h - DECLARATION of the one-time Part 1 JSON import.
//
// The other types are only used through references, so forward declarations are
// enough here: this header does not need to pull in the database or the
// repositories just to declare a function. That is the main reason to split a
// declaration into its own header at all.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

class Database;
class ItemRepo;
class UserRepo;

// Imports whatever is missing and returns human-readable log lines.
// Safe to call on every start: it does nothing once the tables have rows.
std::vector<std::string> importLegacyJson(const std::string& dataDir, Database& db,
                                          UserRepo& users, ItemRepo& items);
