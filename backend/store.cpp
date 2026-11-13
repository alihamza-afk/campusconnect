// ---------------------------------------------------------------------------
// store.cpp - the notices, in memory, for fast reads.
//
// Write order is ALWAYS: database first, memory second. If the database write
// throws, memory is untouched and the request fails cleanly. If the process
// crashes after the database write but before the memory update, the next
// start-up rebuilds memory from the database, so nothing is lost - memory is
// a cache, never the truth.
// ---------------------------------------------------------------------------
#include "store.h"

#include <algorithm>
#include <functional>
#include <set>
#include <vector>

#include "heap.h"
#include "text.h"

// ---------------------------------------------------------------------------
// Sort orders. "before" = "should be popped from the heap first".
// ---------------------------------------------------------------------------
bool Store::newestFirst(const Item& a, const Item& b) {
    if (a.createdAt != b.createdAt) return a.createdAt > b.createdAt;
    return a.id > b.id;
}

// Equal prices (e.g. every lend/lost/found notice is 0) fall back to newest
// first, so the order is always meaningful and the same on every request.
bool Store::cheapestFirst(const Item& a, const Item& b) {
    if (a.price != b.price) return a.price < b.price;
    return newestFirst(a, b);
}

bool Store::dearestFirst(const Item& a, const Item& b) {
    if (a.price != b.price) return a.price > b.price;
    return newestFirst(a, b);
}

bool Store::mayChange(const Item& item, int userId, bool isAdmin) {
    return isAdmin || item.ownerId == userId;
}

// ---------------------------------------------------------------------------
Store::Store(ItemRepo& repo) : repo_(repo) {
    for (const Item& item : repo_.loadAll()) {
        table.put(item);
        trie.insert(item.title, item.id);
        inverted_.insert(item.title, item.id);
    }
}

Item Store::add(Item item) {
    std::lock_guard<std::mutex> lock(mtx);
    item = repo_.insert(item);          // 1. durability
    table.put(item);                    // 2. memory
    trie.insert(item.title, item.id);
    inverted_.insert(item.title, item.id);
    return item;
}

std::optional<Item> Store::find(int id) {
    std::lock_guard<std::mutex> lock(mtx);
    Item* item = table.get(id);
    if (item == nullptr) return std::nullopt;
    return *item;
}

void Store::markClosed(int id) {
    std::lock_guard<std::mutex> lock(mtx);
    Item* item = table.get(id);
    if (item != nullptr) item->status = "closed";
}

void Store::setImageMime(int id, const std::string& mime) {
    std::lock_guard<std::mutex> lock(mtx);
    Item* item = table.get(id);
    if (item != nullptr) item->imageMime = mime;
}

// The permission check happens inside the lock, together with the change, so
// nothing can modify the item between "may this user change it?" and the change.
Store::Result Store::close(int id, int userId, bool isAdmin) {
    std::lock_guard<std::mutex> lock(mtx);
    Item* item = table.get(id);
    if (item == nullptr) return Result::NotFound;
    if (!mayChange(*item, userId, isAdmin)) return Result::Forbidden;
    repo_.setStatus(id, "closed");      // 1. durability
    item->status = "closed";            // 2. memory
    return Result::Ok;
}

Store::Result Store::remove(int id, int userId, bool isAdmin) {
    std::lock_guard<std::mutex> lock(mtx);
    Item* item = table.get(id);
    if (item == nullptr) return Result::NotFound;
    if (!mayChange(*item, userId, isAdmin)) return Result::Forbidden;
    repo_.remove(id);                   // 1. durability
    trie.erase(item->title, id);        // 2. memory
    inverted_.erase(item->title, id);
    table.remove(id);
    return Result::Ok;
}

Store::QueryPage Store::query(const std::string& text, const std::string& type, const std::string& sort,
                              int limit, int offset) {
    std::lock_guard<std::mutex> lock(mtx);

    // ---- keyword filter: exact words (inverted index) + prefixes (trie) ----
    const std::vector<std::string> words = splitWords(text);
    std::set<int> found;
    if (!words.empty()) {
        for (std::size_t i = 0; i < words.size(); i++) {
            // "wallet" is found by the inverted index, "wall" by the trie.
            std::set<int> ids = inverted_.find(words[i]);
            const std::set<int> prefixed = trie.prefix(words[i]);
            ids.insert(prefixed.begin(), prefixed.end());

            if (i == 0) {
                found = ids;
            } else {
                std::set<int> common;
                for (int id : found) {
                    if (ids.count(id)) common.insert(id);
                }
                found = common;
            }
            if (found.empty()) return QueryPage{};   // one word matched nothing
        }
    }

    std::function<bool(const Item&, const Item&)> order = newestFirst;
    if (sort == "cheap") order = cheapestFirst;
    if (sort == "dear") order = dearestFirst;

    // ---- top-K: keep only the (offset + limit) best matching notices ----
    // The heap is ordered "worst first", so when it overflows we pop the worst
    // item. Cost: O(n log K) instead of sorting all n items (O(n log n)), and
    // the memory held is one page, not the whole board.
    const std::size_t keep = static_cast<std::size_t>(offset) + static_cast<std::size_t>(limit);
    auto laterFirst = [&order](const Item& a, const Item& b) { return order(b, a); };
    Heap<Item> worstFirst(laterFirst);

    long long total = 0;
    table.forEach([&](const Item& item) {
        if (item.status != "open") return;
        if (!type.empty() && item.type != type) return;
        if (!words.empty() && found.count(item.id) == 0) return;
        ++total;
        worstFirst.push(item);
        if (static_cast<std::size_t>(worstFirst.size()) > keep) worstFirst.pop();
    });

    // Popping gives worst -> best; reverse it so the page is in the right order.
    std::vector<Item> best;
    best.reserve(static_cast<std::size_t>(worstFirst.size()));
    while (!worstFirst.empty()) best.push_back(worstFirst.pop());
    std::reverse(best.begin(), best.end());

    QueryPage page;
    page.total = total;
    const std::size_t start = static_cast<std::size_t>(offset);
    for (std::size_t i = start; i < best.size() && page.items.size() < static_cast<std::size_t>(limit); i++) {
        page.items.push_back(best[i]);
    }
    return page;
}
