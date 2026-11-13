#pragma once
// ---------------------------------------------------------------------------
// store.h - DECLARATIONS for the notices held in memory.
//
// Two layers exist on purpose:
//   * ItemRepo (SQLite)   = durability. Survives crashes and restarts.
//   * this class          = speed. O(1) lookup by id, keyword search, and the
//                           requested sort order.
//
// Phase 7: three index structures now do three jobs:
//   HashTable     id -> notice                 (O(1) point lookup)
//   Trie          title-word prefixes          (autocomplete / "cal")
//   InvertedIndex exact title words            (whole-word keyword search)
//
// Queries are paginated with a bounded top-K heap: only the best
// (offset + limit) notices are ever kept, so a page no longer sorts the whole
// board. Write order is still: database first, memory second.
// ---------------------------------------------------------------------------
#include <list>
#include <mutex>
#include <optional>
#include <string>

#include "hashtable.h"
#include "inverted_index.h"
#include "item.h"
#include "item_repo.h"
#include "trie.h"

class Store {
public:
    enum class Result { Ok, NotFound, Forbidden };

    struct QueryPage {
        std::list<Item> items;  // one page, already in the requested order
        long long total = 0;    // how many notices match the filter/search
    };

    // Rebuilds every in-memory index from the database.
    explicit Store(ItemRepo& repo);

    Item add(Item item);

    // A copy of one notice (with its private fields), or nothing.
    std::optional<Item> find(int id);

    // The claim approval transaction already closed the row in the database;
    // this updates the in-memory copy so the board hides it.
    void markClosed(int id);

    // Phase 8: sets the image MIME type on the in-memory item after the database
    // has already been updated.
    void setImageMime(int id, const std::string& mime);

    Result close(int id, int userId, bool isAdmin);
    Result remove(int id, int userId, bool isAdmin);

    // Search + filter + sort, then return only items [offset, offset+limit).
    QueryPage query(const std::string& text, const std::string& type, const std::string& sort,
                    int limit, int offset);

private:
    ItemRepo& repo_;
    HashTable table;          // id -> Item
    Trie trie;                // title prefixes -> ids
    InvertedIndex inverted_;  // title words -> ids
    // cpp-httplib runs handlers on a thread pool, so every public method takes
    // this lock before touching the indexes or the database.
    std::mutex mtx;

    // Sort orders: "before" means "comes first in the output".
    static bool newestFirst(const Item& a, const Item& b);
    static bool cheapestFirst(const Item& a, const Item& b);
    static bool dearestFirst(const Item& a, const Item& b);
    // Only the person who posted an item, or an admin, may close or delete it.
    static bool mayChange(const Item& item, int userId, bool isAdmin);
};
