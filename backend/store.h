#pragma once
#include <ctime>
#include <list>
#include <string>
#include <functional>
#include <mutex>
#include "hashtable.h"
#include "trie.h"
#include "heap.h"
#include "storage.h"

class Store {
public:
    enum class Result { Ok, NotFound, Forbidden };

private:
    HashTable table;
    Trie trie;
    int nextId = 1;
    std::string path;   // JSON file the items are saved to; empty = keep in memory only
    // cpp-httplib runs handlers on a thread pool, so every public method
    // takes this lock before touching table, trie or nextId.
    std::mutex mtx;

    static bool newestFirst(const Item& a, const Item& b) {
        if (a.createdAt != b.createdAt) return a.createdAt > b.createdAt;
        return a.id > b.id;
    }

    // Equal prices (e.g. every lend/lost/found notice is 0) fall back to newest first,
    // so the order is always meaningful and the same on every request.
    static bool cheapestFirst(const Item& a, const Item& b) {
        if (a.price != b.price) return a.price < b.price;
        return newestFirst(a, b);
    }

    static bool dearestFirst(const Item& a, const Item& b) {
        if (a.price != b.price) return a.price > b.price;
        return newestFirst(a, b);
    }

    // Only the person who posted an item, or an admin, may close or delete it.
    static bool mayChange(const Item& item, int userId, bool isAdmin) {
        return isAdmin || item.ownerId == userId;
    }

    void saveLocked() {
        if (path.empty()) return;
        json list = json::array();
        table.forEach([&](const Item& it) { list.push_back(itemToJson(it)); });
        writeJsonFileAtomic(path, json{{"nextId", nextId}, {"items", list}});
    }

public:
    explicit Store(std::string file = "") : path(std::move(file)) {
        if (path.empty()) return;
        json data;
        if (!readJsonFile(path, data)) return;
        nextId = data.value("nextId", 1);
        for (const json& j : data.at("items")) {
            Item it = itemFromJson(j);
            table.put(it);
            trie.insert(it.title, it.id);
            if (it.id >= nextId) nextId = it.id + 1;
        }
    }

    Item add(Item item) {
        std::lock_guard<std::mutex> lock(mtx);
        item.id = nextId++;
        item.createdAt = std::time(nullptr);
        item.status = "open";
        table.put(item);
        trie.insert(item.title, item.id);
        saveLocked();
        return item;
    }

    // The permission check happens inside the lock, together with the change,
    // so nothing can modify the item between "may this user change it?" and the change itself.
    Result close(int id, int userId, bool isAdmin) {
        std::lock_guard<std::mutex> lock(mtx);
        Item* item = table.get(id);
        if (item == nullptr) return Result::NotFound;
        if (!mayChange(*item, userId, isAdmin)) return Result::Forbidden;
        item->status = "closed";
        saveLocked();
        return Result::Ok;
    }

    Result remove(int id, int userId, bool isAdmin) {
        std::lock_guard<std::mutex> lock(mtx);
        Item* item = table.get(id);
        if (item == nullptr) return Result::NotFound;
        if (!mayChange(*item, userId, isAdmin)) return Result::Forbidden;
        trie.erase(item->title, id);
        table.remove(id);
        saveLocked();
        return Result::Ok;
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
