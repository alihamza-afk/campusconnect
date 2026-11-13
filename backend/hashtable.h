#pragma once
// ---------------------------------------------------------------------------
// hashtable.h - DECLARATIONS for the chaining hash table (id -> notice).
//
// Phase 7 change: the table grows. Before, it had a fixed 101 buckets and the
// chains got longer as notices were added. Now it tracks its load factor and
// rehashes into a bigger prime-sized table when the load passes 0.75.
//
// That turns the worst case from "chains grow without limit" into the textbook
// behaviour: O(1) average lookup, and the occasional O(n) rehash amortised over
// the inserts that caused it.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <functional>
#include <list>
#include <vector>

#include "item.h"

class HashTable {
    static const int INITIAL_BUCKETS = 101;
    static constexpr double MAX_LOAD_FACTOR = 0.75;

    std::vector<std::list<Item>> buckets_;  // each bucket is a collision chain
    std::size_t count_ = 0;                 // items stored, for the load factor

    int hashOf(int id) const;               // id % bucket count
    void rehashIfNeeded();
    void rehash(std::size_t newBucketCount);
    static std::size_t nextPrime(std::size_t n);

public:
    HashTable();

    void put(const Item& item);   // insert, or replace the item with the same id
    Item* get(int id);            // nullptr when not found
    bool remove(int id);
    void forEach(const std::function<void(const Item&)>& fn) const;

    // Small observability helpers: useful for tests and for explaining growth.
    std::size_t size() const;
    std::size_t bucketCount() const;
};
