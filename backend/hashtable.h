#pragma once
#include <list>
#include <functional>
#include "item.h"

class HashTable {
    static const int SIZE = 101;
    std::list<Item> table[SIZE];

    int hashOf(int id) const {
        return id % SIZE;
    }

public:
    void put(const Item& item) {
        std::list<Item>& bucket = table[hashOf(item.id)];
        for (Item& x : bucket) {
            if (x.id == item.id) {
                x = item;
                return;
            }
        }
        bucket.push_back(item);
    }

    Item* get(int id) {
        for (Item& x : table[hashOf(id)]) {
            if (x.id == id) return &x;
        }
        return nullptr;
    }

    bool remove(int id) {
        std::list<Item>& bucket = table[hashOf(id)];
        for (auto it = bucket.begin(); it != bucket.end(); ++it) {
            if (it->id == id) {
                bucket.erase(it);
                return true;
            }
        }
        return false;
    }

    void forEach(const std::function<void(const Item&)>& fn) const {
        for (int i = 0; i < SIZE; i++) {
            for (const Item& x : table[i]) fn(x);
        }
    }
};
