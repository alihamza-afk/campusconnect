#pragma once
#include <ctime>
#include <list>
#include <string>
#include <functional>
#include <mutex>
#include "hashtable.h"
#include "trie.h"
#include "heap.h"

class Store {
    HashTable table;
    Trie trie;
    int nextId = 1;
    // cpp-httplib runs handlers on a thread pool, so every public method
    // takes this lock before touching table, trie or nextId.
    std::mutex mtx;

    static bool newestFirst(const Item& a, const Item& b) {
        if (a.createdAt != b.createdAt) return a.createdAt > b.createdAt;
        return a.id > b.id;
    }

    static bool cheapestFirst(const Item& a, const Item& b) {
        return a.price < b.price;
    }

    static bool dearestFirst(const Item& a, const Item& b) {
        return a.price > b.price;
    }

public:
    Item add(Item item) {
        std::lock_guard<std::mutex> lock(mtx);
        item.id = nextId++;
        item.createdAt = std::time(nullptr);
        item.status = "open";
        table.put(item);
        trie.insert(item.title, item.id);
        return item;
    }

    bool close(int id) {
        std::lock_guard<std::mutex> lock(mtx);
        Item* item = table.get(id);
        if (item == nullptr) return false;
        item->status = "closed";
        return true;
    }

    bool remove(int id) {
        std::lock_guard<std::mutex> lock(mtx);
        Item* item = table.get(id);
        if (item == nullptr) return false;
        trie.erase(item->title, id);
        return table.remove(id);
    }

    std::list<Item> query(const std::string& text, const std::string& type, const std::string& sort) {
        std::lock_guard<std::mutex> lock(mtx);
        bool searching = !text.empty();
        std::set<int> found;
        if (searching) found = trie.search(text);

        std::function<bool(const Item&, const Item&)> order = newestFirst;
        if (sort == "cheap") order = cheapestFirst;
        if (sort == "dear") order = dearestFirst;

        Heap<Item> heap(order);
        table.forEach([&](const Item& item) {
            if (item.status != "open") return;
            if (!type.empty() && item.type != type) return;
            if (searching && found.count(item.id) == 0) return;
            heap.push(item);
        });

        std::list<Item> result;
        while (!heap.empty()) result.push_back(heap.pop());
        return result;
    }
};
