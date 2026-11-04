#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sodium.h>
#include "httplib.h"
#include "storage.h"
#include "store.h"
#include "auth.h"

// ---------- configuration (environment variables) ----------
//   CC_DATA_DIR        where items.json and users.json are kept         (default: data)
//   CC_FRONTEND_DIR    folder served as the website                      (default: ../frontend)
//   CC_EMAIL_DOMAIN    only allow sign-up with this email domain, e.g. geu.ac.in (default: any)
//   CC_COOKIE_SECURE   set to 1 when served over HTTPS, so the cookie is never sent over plain HTTP
//   CC_ADMIN_EMAIL / CC_ADMIN_NAME / CC_ADMIN_PASSWORD   creates the first admin if none exists
static std::string env(const char* key, const char* fallback = "") {
    const char* v = std::getenv(key);
    return v ? v : fallback;
}

static const char* COOKIE_NAME = "cc_session";
static const std::set<std::string> CATEGORIES = {"Books", "Electronics", "Stationery", "Clothing", "Cycle", "ID or wallet", "Other"};

// ---------- helpers ----------
void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(), "application/json");
}

void replyError(httplib::Response& res, int status, const std::string& message) {
    reply(res, status, json{{"error", message}});
}

json userJson(const User& u) {
    return json{{"id", u.id}, {"name", u.name}, {"email", u.email}, {"role", u.role}};
}

// Reads a string field; anything that is missing or not a string counts as "".
std::string str(const json& body, const char* key) {
    auto it = body.find(key);
    return (it != body.end() && it->is_string()) ? it->get<std::string>() : "";
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string cookieValue(const httplib::Request& req, const std::string& name) {
    std::string header = req.get_header_value("Cookie");
    size_t pos = 0;
    while (pos < header.size()) {
        size_t end = header.find(';', pos);
        if (end == std::string::npos) end = header.size();
        std::string part = trim(header.substr(pos, end - pos));
        if (part.compare(0, name.size() + 1, name + "=") == 0) return part.substr(name.size() + 1);
        pos = end + 1;
    }
    return "";
}

// HttpOnly: page scripts cannot read the token, so an injected script cannot steal it.
// SameSite=Strict: the browser does not attach it to requests started by other websites (CSRF).
void setSessionCookie(httplib::Response& res, const std::string& token, long long maxAge, bool secure) {
    std::string c = std::string(COOKIE_NAME) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(maxAge);
    if (secure) c += "; Secure";
    res.set_header("Set-Cookie", c);
}

int parseId(const std::string& s) {
    if (s.empty() || s.size() > 9) return -1;   // \d+ in the route already guarantees digits
    return std::stoi(s);
}

// Validates and cleans a new item. Returns an error message, or "" if it is fine.
std::string validateItem(Item& item) {
    item.title = trim(item.title);
    item.description = trim(item.description);
    item.location = trim(item.location);
    if (item.type != "sell" && item.type != "lend" && item.type != "lost" && item.type != "found") return "choose what the notice is for";
    if (item.title.empty() || utf8Length(item.title) > 60) return "title must be 1 to 60 characters";
    if (utf8Length(item.description) > 200) return "details must be at most 200 characters";
    if (utf8Length(item.location) > 60) return "location must be at most 60 characters";
    if (!CATEGORIES.count(item.category)) item.category = "Other";
    if (item.type != "sell") item.price = 0;
    if (!std::isfinite(item.price) || item.price < 0 || item.price > 1000000) return "price must be between 0 and 1000000";
    return "";
}

int main() {
    if (sodium_init() < 0) {
        std::cerr << "libsodium failed to initialise" << std::endl;
        return 1;
    }

    const std::string dataDir = env("CC_DATA_DIR", "data");
    const bool secureCookie = env("CC_COOKIE_SECURE") == "1";

    Store store(dataDir + "/items.json");
    Auth auth(dataDir + "/users.json");
    auth.setAllowedDomain(env("CC_EMAIL_DOMAIN"));

    std::string adminMsg = auth.ensureAdmin(env("CC_ADMIN_EMAIL"), env("CC_ADMIN_NAME", "Admin"), env("CC_ADMIN_PASSWORD"));
    if (!adminMsg.empty()) std::cout << adminMsg << std::endl;

    httplib::Server server;
    server.set_payload_max_length(16 * 1024);   // notices are tiny; refuse large request bodies

    // No Access-Control-Allow-Origin header: the frontend is served by this same server,
    // so other websites have no reason to call this API from a browser.
    server.set_default_headers({
        {"X-Content-Type-Options", "nosniff"},
        {"X-Frame-Options", "DENY"},
        {"Referrer-Policy", "same-origin"},
        {"Content-Security-Policy",
         "default-src 'self'; style-src 'self' https://fonts.googleapis.com; "
         "font-src https://fonts.gstatic.com; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}
    });

    // Second layer against cross-site requests (SameSite is the first): browsers send an Origin
    // header on POST/DELETE, and it must match the host the request was sent to.
    server.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.method == "GET" || req.method == "HEAD" || !req.has_header("Origin")) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        std::string origin = req.get_header_value("Origin");
        std::string host = req.get_header_value("Host");
        if (origin == "http://" + host || origin == "https://" + host) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        replyError(res, 403, "cross-site request blocked");
        return httplib::Server::HandlerResponse::Handled;
    });

    // Fills `user` from the session cookie, or replies 401 and returns false.
    auto requireUser = [&](const httplib::Request& req, httplib::Response& res, User& user) {
        if (auth.userForToken(cookieValue(req, COOKIE_NAME), user)) return true;
        replyError(res, 401, "please log in");
        return false;
    };

    auto parseBody = [](const httplib::Request& req, httplib::Response& res, json& body) {
        body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            replyError(res, 400, "invalid JSON");
            return false;
        }
        return true;
    };

    // ---------- accounts ----------
    server.Post("/api/register", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        User user;
        // Role is always "user" here. Admins are created only from server configuration.
        std::string err = auth.registerUser(str(body, "email"), str(body, "name"), str(body, "password"), "user", true, &user);
        if (!err.empty()) return replyError(res, 400, err);
        setSessionCookie(res, auth.startSession(user.id), Auth::SESSION_SECONDS, secureCookie);
        reply(res, 201, userJson(user));
    });

    server.Post("/api/login", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        std::string token;
        User user;
        switch (auth.login(str(body, "email"), str(body, "password"), token, user)) {
            case Auth::LoginResult::Ok:
                setSessionCookie(res, token, Auth::SESSION_SECONDS, secureCookie);
                return reply(res, 200, userJson(user));
            case Auth::LoginResult::Locked:
                return replyError(res, 429, "too many failed attempts, try again in 15 minutes");
            case Auth::LoginResult::Bad:
                // Same message whether the email or the password was wrong.
                return replyError(res, 401, "invalid email or password");
        }
    });

    server.Post("/api/logout", [&](const httplib::Request& req, httplib::Response& res) {
        auth.logout(cookieValue(req, COOKIE_NAME));
        setSessionCookie(res, "", 0, secureCookie);
        reply(res, 200, json{{"ok", true}});
    });

    server.Get("/api/me", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        reply(res, 200, userJson(user));
    });

    // ---------- items (all require login) ----------
    server.Get("/api/items", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        std::string q = req.get_param_value("q");
        std::string type = req.get_param_value("type");
        std::string sort = req.has_param("sort") ? req.get_param_value("sort") : "new";

        json list = json::array();
        for (const Item& it : store.query(q, type, sort)) {
            json j = itemToJson(it);
            // Tells the page which buttons to show. The server still checks again on every action.
            j["canClose"] = (user.role == "admin" || it.ownerId == user.id);
            list.push_back(j);
        }
        reply(res, 200, list);
    });

    server.Post("/api/items", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        json body;
        if (!parseBody(req, res, body)) return;

        Item item;
        item.type = str(body, "type");
        item.title = str(body, "title");
        item.description = str(body, "description");
        item.category = str(body, "category");
        item.location = str(body, "location");
        auto price = body.find("price");
        item.price = (price != body.end() && price->is_number()) ? price->get<double>() : 0.0;
        // The poster is whoever is logged in. Any "owner" sent by the browser is ignored.
        item.owner = user.name;
        item.ownerId = user.id;

        std::string err = validateItem(item);
        if (!err.empty()) return replyError(res, 400, err);
        reply(res, 201, itemToJson(store.add(item)));
    });

    auto sendResult = [](httplib::Response& res, Store::Result r) {
        if (r == Store::Result::Ok) reply(res, 200, json{{"ok", true}});
        else if (r == Store::Result::Forbidden) replyError(res, 403, "only the person who posted this or an admin can change it");
        else replyError(res, 404, "not found");
    };

    server.Post(R"(/api/items/(\d+)/close)", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        int id = parseId(req.matches[1]);
        if (id < 0) return replyError(res, 404, "not found");
        sendResult(res, store.close(id, user.id, user.role == "admin"));
    });

    server.Delete(R"(/api/items/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        int id = parseId(req.matches[1]);
        if (id < 0) return replyError(res, 404, "not found");
        sendResult(res, store.remove(id, user.id, user.role == "admin"));
    });

    server.set_mount_point("/", env("CC_FRONTEND_DIR", "../frontend"));

    std::cout << "CampusConnect running on http://localhost:8080" << std::endl;
    server.listen("0.0.0.0", 8080);
}
