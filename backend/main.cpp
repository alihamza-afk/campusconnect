#include <iostream>
#include "httplib.h"
#include "json.hpp"
#include "store.h"

using json = nlohmann::json;

json toJson(const Item& it) {
    json j;
    j["id"] = it.id;
    j["type"] = it.type;
    j["title"] = it.title;
    j["description"] = it.description;
    j["category"] = it.category;
    j["location"] = it.location;
    j["owner"] = it.owner;
    j["price"] = it.price;
    j["createdAt"] = it.createdAt;
    j["status"] = it.status;
    return j;
}

void reply(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_content(body, "application/json");
}

int main() {
    Store store;
    httplib::Server server;

    store.add({0, "sell", "Engineering Mathematics Vol 2", "Barely used, no markings", "Books", "Hostel B", "Riya", 250});
    store.add({0, "lend", "Scientific Calculator fx-991", "Can lend for exams, return in 3 days", "Electronics", "Library", "Aman", 0});
    store.add({0, "lost", "Black wallet", "Lost near the canteen, has college ID", "ID or wallet", "Canteen", "Kabir", 0});

    server.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "Content-Type"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"}
    });

    server.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    server.Get("/api/items", [&](const httplib::Request& req, httplib::Response& res) {
        std::string q = req.get_param_value("q");
        std::string type = req.get_param_value("type");
        std::string sort = req.has_param("sort") ? req.get_param_value("sort") : "new";

        json list = json::array();
        for (const Item& it : store.query(q, type, sort)) {
            list.push_back(toJson(it));
        }
        reply(res, 200, list.dump());
    });

    server.Post("/api/items", [&](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            reply(res, 400, R"({"error":"invalid JSON"})");
            return;
        }

        Item item;
        item.type = body.value("type", "");
        item.title = body.value("title", "");
        item.description = body.value("description", "");
        item.category = body.value("category", "Other");
        item.location = body.value("location", "");
        item.owner = body.value("owner", "");
        item.price = body.value("price", 0.0);

        bool typeOk = item.type == "sell" || item.type == "lend" || item.type == "lost" || item.type == "found";
        if (!typeOk || item.title.empty() || item.owner.empty()) {
            reply(res, 400, R"({"error":"type, title and owner are required"})");
            return;
        }
        if (item.type != "sell") item.price = 0;

        Item saved = store.add(item);
        reply(res, 201, toJson(saved).dump());
    });

    server.Post(R"(/api/items/(\d+)/close)", [&](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        if (store.close(id)) reply(res, 200, R"({"ok":true})");
        else reply(res, 404, R"({"error":"not found"})");
    });

    server.Delete(R"(/api/items/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        if (store.remove(id)) reply(res, 200, R"({"ok":true})");
        else reply(res, 404, R"({"error":"not found"})");
    });

    server.set_mount_point("/", "../frontend");

    std::cout << "CampusConnect running on http://localhost:8080" << std::endl;
    server.listen("0.0.0.0", 8080);
}
