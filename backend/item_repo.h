#pragma once
// ---------------------------------------------------------------------------
// item_repo.h - DECLARATIONS for the SQL that touches the `items` table.
//
// The in-memory DSA structures (HashTable, Trie, Heap) are NOT the source of
// truth. This class is. On start-up Store() loads every row through loadAll()
// and rebuilds the indexes; on every write the repository saves to disk first,
// then Store updates memory. That order is what makes a restart safe.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

#include "db.h"
#include "item.h"

class ItemRepo {
    Database& db_;

public:
    explicit ItemRepo(Database& db);

    // Reads the whole table once, at start-up, to rebuild the in-memory indexes.
    std::vector<Item> loadAll();

    // Saves a new notice and returns it with the id and timestamp the database
    // assigned. The caller must already have validated every field.
    Item insert(Item item);

    bool setStatus(int id, const std::string& status);
    bool remove(int id);

    // Phase 8: store the MIME type of an attached image.
    bool setImageMime(int id, const std::string& mime);

    // Phase 8: stats helpers for the admin dashboard.
    long long countOpen();
    long long countClosed();
    long long countByType(const std::string& type);
};
