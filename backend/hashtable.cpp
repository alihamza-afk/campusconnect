// ---------------------------------------------------------------------------
// hashtable.cpp - chaining hash table that rehashes as it fills.
//
// Why grow? A chain's length is roughly count / buckets. A fixed 101-bucket
// table is fine for ~75 notices and becomes 10-item chains at 1000 notices.
// When the load factor passes 0.75 we build a table about twice as large and
// move every item into it. The move is O(n), but it happens rarely, so the cost
// per insert stays O(1) on average (amortised analysis).
//
// Growing to a prime size keeps the simple "id % bucketCount" hash well spread
// for sequential ids (with a power-of-two size, even ids would all land in even
// buckets).
// ---------------------------------------------------------------------------
#include "hashtable.h"

#include <utility>

HashTable::HashTable() {
    buckets_.resize(INITIAL_BUCKETS);
}

int HashTable::hashOf(int id) const {
    return id % static_cast<int>(buckets_.size());
}

std::size_t HashTable::nextPrime(std::size_t n) {
    if (n < 2) return 2;
    for (std::size_t candidate = n | 1;; candidate += 2) {   // odd candidates only
        bool prime = true;
        for (std::size_t divisor = 3; divisor * divisor <= candidate; divisor += 2) {
            if (candidate % divisor == 0) {
                prime = false;
                break;
            }
        }
        if (prime) return candidate;
    }
}

void HashTable::rehash(std::size_t newBucketCount) {
    std::vector<std::list<Item>> fresh(newBucketCount);
    for (auto& bucket : buckets_) {
        for (Item& item : bucket) {
            fresh[static_cast<std::size_t>(item.id) % newBucketCount].push_back(std::move(item));
        }
    }
    buckets_ = std::move(fresh);
}

void HashTable::rehashIfNeeded() {
    const double load = static_cast<double>(count_) / static_cast<double>(buckets_.size());
    if (load <= MAX_LOAD_FACTOR) return;
    rehash(nextPrime(buckets_.size() * 2 + 1));
}

void HashTable::put(const Item& item) {
    std::list<Item>& bucket = buckets_[hashOf(item.id)];
    for (Item& existing : bucket) {
        if (existing.id == item.id) {
            existing = item;  // replace, never duplicate
            return;
        }
    }
    bucket.push_back(item);
    ++count_;
    rehashIfNeeded();
}

Item* HashTable::get(int id) {
    for (Item& item : buckets_[hashOf(id)]) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

bool HashTable::remove(int id) {
    std::list<Item>& bucket = buckets_[hashOf(id)];
    for (auto it = bucket.begin(); it != bucket.end(); ++it) {
        if (it->id == id) {
            bucket.erase(it);
            --count_;
            return true;
        }
    }
    return false;
}

void HashTable::forEach(const std::function<void(const Item&)>& fn) const {
    for (const std::list<Item>& bucket : buckets_) {
        for (const Item& item : bucket) fn(item);
    }
}

std::size_t HashTable::size() const { return count_; }

std::size_t HashTable::bucketCount() const { return buckets_.size(); }
