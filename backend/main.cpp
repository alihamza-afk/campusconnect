#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sodium.h>

#include "auth.h"
#include "claim_repo.h"
#include "db.h"
#include "email_service.h"
#include "email_token_repo.h"
#include "httplib.h"
#include "images.h"
#include "mail_queue.h"
#include "mailer.h"
#include "notification_repo.h"
#include "rate_limiter.h"
#include "item_repo.h"
#include "json.hpp"
#include "legacy_json.h"
#include "schema.h"
#include "session_repo.h"
#include "store.h"
#include "user_repo.h"

using json = nlohmann::json;

// ---------- configuration (environment variables) ----------
//   CC_DATA_DIR        folder holding campus.db (and the old part-1 JSON files)   (default: data)
//   CC_FRONTEND_DIR    folder served as the website                               (default: ../frontend)
//   CC_EMAIL_DOMAIN    only allow sign-up with this email domain, e.g. geu.ac.in  (default: any)
//   CC_COOKIE_SECURE   set to 1 when served over HTTPS, so the cookie is never sent over plain HTTP
//   CC_PORT            TCP port to listen on                                       (default: 8080)
//   CC_MAIL_MODE       console (prints emails, works offline) or brevo (sends them) (default: console)
//   CC_BASE_URL        base URL used in emailed links, e.g. https://campus.example  (default: http://localhost:<port>)
//   CC_BREVO_API_KEY   Brevo API key when CC_MAIL_MODE=brevo
//   CC_MAIL_FROM / CC_MAIL_FROM_NAME   sender address/name; Brevo requires a verified sender address
//   CC_TRUST_PROXY     set to 1 only when a reverse proxy you control forwards requests,
//                      so X-Forwarded-For can be believed for the per-IP rate limits
//   CC_HSTS            set to 1 when served over HTTPS, to send HSTS (never over plain HTTP)
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
    return json{{"id", u.id}, {"name", u.name}, {"email", u.email}, {"role", u.role},
                {"emailVerified", u.emailVerifiedAt != 0},
                {"banned", u.bannedAt != 0}};
}

// One place that decides what a notice looks like on the wire. The database
// column names (owner_id, created_at) never leak to the frontend; the JSON
// names stay exactly as Part 1's frontend expects them.
json itemJson(const Item& it) {
    json j = json{
        {"id", it.id}, {"type", it.type}, {"title", it.title},
        {"description", it.description}, {"category", it.category},
        {"location", it.location}, {"owner", it.owner}, {"ownerId", it.ownerId},
        {"price", it.price}, {"createdAt", it.createdAt}, {"status", it.status},
        {"hasImage", !it.imageMime.empty()}
    };
    return j;
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

// The real client address. Behind a reverse proxy the socket address is the
// proxy, so X-Forwarded-For is used only when the operator explicitly says the
// proxy is trusted (CC_TRUST_PROXY=1). Trusting it by default would let any
// client spoof an address and dodge the per-IP rate limit.
std::string clientIp(const httplib::Request& req, bool trustProxy) {
    if (trustProxy && req.has_header("X-Forwarded-For")) {
        std::string forwarded = req.get_header_value("X-Forwarded-For");
        const size_t comma = forwarded.find(',');
        if (comma != std::string::npos) forwarded = forwarded.substr(0, comma);
        forwarded = trim(forwarded);
        if (!forwarded.empty()) return forwarded;
    }
    return req.remote_addr;
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
    // The private claim fields only make sense for lost & found notices.
    item.proof = trim(item.proof);
    item.contact = trim(item.contact);
    if (item.type != "lost" && item.type != "found") {
        item.proof.clear();
        item.contact.clear();
    }
    if (utf8Length(item.proof) > 120) return "the private detail must be at most 120 characters";
    if (utf8Length(item.contact) > 100) return "the contact field must be at most 100 characters";
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
    const bool trustProxy = env("CC_TRUST_PROXY") == "1";
    int port = 8080;
    try {
        port = std::stoi(env("CC_PORT", "8080"));
    } catch (const std::exception&) {
        std::cerr << "CC_PORT is not a number, using 8080" << std::endl;
    }

    // Base URL used inside emailed links. Defaults to the local server, so the
    // console mailer prints a clickable link during a demo.
    const std::string defaultBaseUrl = "http://localhost:" + std::to_string(port);
    const std::string baseUrl = env("CC_BASE_URL", defaultBaseUrl.c_str());
    std::unique_ptr<Mailer> rawMailer = makeMailer(
        env("CC_MAIL_MODE", "console"), env("CC_BREVO_API_KEY"),
        env("CC_MAIL_FROM", "no-reply@campusconnect.local"), env("CC_MAIL_FROM_NAME", "CampusConnect"));

    // The mail queue wraps the real mailer: HTTP handlers push a message and
    // return immediately; the worker thread delivers it in the background.
    MailQueue mailQueue(*rawMailer);

    // ---------- database ----------
    // The data folder must exist before SQLite can create the file inside it.
    std::filesystem::create_directories(std::filesystem::path(dataDir));
    // The images sub-folder holds uploaded item photos.
    const std::string imagesDir = dataDir + "/images";
    std::filesystem::create_directories(std::filesystem::path(imagesDir));

    Database db(dataDir + "/campus.db");
    migrate(db);

    UserRepo userRepo(db);
    ItemRepo itemRepo(db);
    ClaimRepo claimRepo(db);
    RateLimiter rateLimiter(db);
    SessionRepo sessionRepo(db);
    EmailTokenRepo emailTokenRepo(db);
    NotificationRepo notifRepo(db);
    // EmailService uses the queue as its Mailer, so verification emails are
    // sent asynchronously.
    EmailService emailService(emailTokenRepo, userRepo, sessionRepo, mailQueue, baseUrl);

    // Part 1 saved JSON files; copy them in once if the database is still empty.
    for (const std::string& line : importLegacyJson(dataDir, db, userRepo, itemRepo)) {
        std::cout << line << std::endl;
    }

    // Both read from the database: Store rebuilds its indexes, Auth loads no user table.
    Store store(itemRepo);
    Auth auth(userRepo, sessionRepo, rateLimiter);
    auth.setAllowedDomain(env("CC_EMAIL_DOMAIN"));

    std::string adminMsg = auth.ensureAdmin(env("CC_ADMIN_EMAIL"), env("CC_ADMIN_NAME", "Admin"), env("CC_ADMIN_PASSWORD"));
    if (!adminMsg.empty()) std::cout << adminMsg << std::endl;

    httplib::Server server;
    server.set_payload_max_length(5 * 1024 * 1024);  // allow up to 5 MB for image uploads

    // An exception inside a handler must not take the server down. Log the real
    // reason on the server, but never send internal details to the browser.
    server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string message = "unknown error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
        }
        std::cerr << "request failed: " << message << std::endl;
        res.status = 500;
        res.set_header("Cache-Control", "no-store");
        res.set_content(json{{"error", "internal server error"}}.dump(), "application/json");
    });

    // No Access-Control-Allow-Origin header: the frontend is served by this same server,
    // so other websites have no reason to call this API from a browser.
    httplib::Headers defaultHeaders = {
        {"X-Content-Type-Options", "nosniff"},
        {"X-Frame-Options", "DENY"},
        {"Referrer-Policy", "same-origin"},
        {"Content-Security-Policy",
         "default-src 'self'; style-src 'self' https://fonts.googleapis.com; "
         "font-src https://fonts.gstatic.com; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}};
    // HSTS tells the browser to use HTTPS for a year. It must only be sent when
    // the site really is HTTPS - over plain HTTP it would lock users out.
    if (env("CC_HSTS") == "1") {
        defaultHeaders.emplace("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
    }
    server.set_default_headers(defaultHeaders);

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

    auto requireAdmin = [&](const httplib::Request& req, httplib::Response& res, User& user) {
        if (!auth.userForToken(cookieValue(req, COOKIE_NAME), user)) {
            replyError(res, 401, "please log in");
            return false;
        }
        if (user.role != "admin") {
            replyError(res, 403, "admin only");
            return false;
        }
        return true;
    };

    auto parseBody = [](const httplib::Request& req, httplib::Response& res, json& body) {
        body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            replyError(res, 400, "invalid JSON");
            return false;
        }
        return true;
    };

    // Counts one hit for a key and says whether the caller is already over the
    // limit. A blocked caller does not count again, so the window cannot be
    // extended forever by keep trying.
    auto rateLimited = [&](const std::string& key, int limit, long long windowSeconds) {
        const long long t = static_cast<long long>(std::time(nullptr));
        if (rateLimiter.count(key, windowSeconds, t) >= limit) return true;
        rateLimiter.record(key, windowSeconds, t);
        return false;
    };

    // ---------- accounts ----------
    server.Post("/api/register", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        // 10 new accounts per network per hour.
        if (rateLimited("register-ip:" + clientIp(req, trustProxy), 10, 3600)) {
            return replyError(res, 429, "too many sign-ups from this network, try later");
        }
        User user;
        // Role is always "user" here. Admins are created only from server configuration.
        std::string err = auth.registerUser(str(body, "email"), str(body, "name"), str(body, "password"), "user", true, &user);
        if (!err.empty()) return replyError(res, 400, err);
        // The account is usable for browsing immediately, but posting waits
        // until the email link is clicked.
        if (emailService.sendVerification(user) == EmailService::SendResult::Failed) {
            std::cerr << "could not send the verification email to " << user.email << std::endl;
        }
        setSessionCookie(res, auth.startSession(user.id), Auth::SESSION_SECONDS, secureCookie);
        reply(res, 201, userJson(user));
    });

    server.Post("/api/login", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        std::string token;
        User user;
        switch (auth.login(str(body, "email"), str(body, "password"), clientIp(req, trustProxy), token, user)) {
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

    // Ends every session for the logged-in account, on every device.
    server.Post("/api/logout-all", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        const int ended = auth.logoutAll(user.id);
        setSessionCookie(res, "", 0, secureCookie);
        reply(res, 200, json{{"ok", true}, {"ended", ended}});
    });

    server.Get("/api/me", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        // Include the unread notification count so the bell can update without a
        // separate round-trip.
        const int unread = notifRepo.unreadCount(user.id);
        json j = userJson(user);
        j["unreadNotifications"] = unread;
        reply(res, 200, j);
    });

    // ---------- email verification and password reset ----------
    server.Post("/api/verify", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        switch (emailService.verify(str(body, "token"))) {
            case EmailService::VerifyResult::Ok:
                return reply(res, 200, json{{"ok", true}});
            case EmailService::VerifyResult::Invalid:
            case EmailService::VerifyResult::Expired:
                return replyError(res, 400, "this link is invalid, already used, or has expired");
        }
    });

    server.Post("/api/verify/resend", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        if (user.emailVerifiedAt != 0) return replyError(res, 400, "your email is already verified");
        if (rateLimited("resend-ip:" + clientIp(req, trustProxy), 20, 3600)) {
            return replyError(res, 429, "too many requests from this network, try later");
        }
        switch (emailService.sendVerification(user)) {
            case EmailService::SendResult::Sent:
                return reply(res, 200, json{{"ok", true}});
            case EmailService::SendResult::TooSoon:
                return replyError(res, 429, "a link was just sent; wait a minute before asking again");
            case EmailService::SendResult::Failed:
                return replyError(res, 503, "could not send the email right now");
        }
    });

    // Always answers the same way: whether an account exists must not leak.
    server.Post("/api/forgot", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        const std::string email = Auth::normalizeEmail(str(body, "email"));
        const long long t = static_cast<long long>(std::time(nullptr));
        const std::string ip = clientIp(req, trustProxy);

        // 10 requests per network per hour, at most 3 links per address per hour.
        // The answer is identical in every case: whether the address exists, and
        // whether a link was actually sent, must not leak.
        const bool ipAllowed = rateLimiter.count("forgot-ip:" + ip, 3600, t) < 10;
        if (ipAllowed) rateLimiter.record("forgot-ip:" + ip, 3600, t);
        if (ipAllowed && !email.empty()) {
            std::optional<User> user = userRepo.findByEmail(email);
            if (user) {
                const std::string emailKey = "forgot-email:" + email;
                if (rateLimiter.count(emailKey, 3600, t) < 3) {
                    rateLimiter.record(emailKey, 3600, t);
                    emailService.sendPasswordReset(*user);
                }
            }
        }
        reply(res, 200, json{{"ok", true},
                             {"message", "if that address has an account, a reset link is on its way"}});
    });

    server.Post("/api/reset", [&](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parseBody(req, res, body)) return;
        switch (emailService.resetPassword(str(body, "token"), str(body, "password"))) {
            case EmailService::ResetResult::Ok:
                // Every session was revoked server-side; clear the cookie too.
                setSessionCookie(res, "", 0, secureCookie);
                return reply(res, 200, json{{"ok", true}});
            case EmailService::ResetResult::InvalidToken:
                return replyError(res, 400, "this reset link is invalid, already used, or has expired");
            case EmailService::ResetResult::WeakPassword:
                return replyError(res, 400, "password must be at least 8 characters");
        }
    });

    // ---------- items (all require login) ----------
    server.Get("/api/items", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        const std::string q = req.get_param_value("q");
        const std::string type = req.get_param_value("type");
        const std::string sort = req.has_param("sort") ? req.get_param_value("sort") : "new";

        // Pagination: one page of notices at a time. The store only ever keeps
        // the best (offset + limit) matches, so a page does not sort the board.
        auto intParam = [&](const char* name, int fallback, bool& ok) {
            ok = true;
            if (!req.has_param(name)) return fallback;
            const std::string raw = req.get_param_value(name);
            try {
                std::size_t used = 0;
                const long value = std::stol(raw, &used);
                if (used != raw.size()) { ok = false; return fallback; }
                return static_cast<int>(value);
            } catch (const std::exception&) {
                ok = false;
                return fallback;
            }
        };

        bool ok = false;
        const int limit = intParam("limit", 20, ok);
        if (!ok || limit < 1 || limit > 100) return replyError(res, 400, "limit must be 1 to 100");
        const int offset = intParam("offset", 0, ok);
        if (!ok || offset < 0) return replyError(res, 400, "offset must be 0 or more");

        const Store::QueryPage page = store.query(q, type, sort, limit, offset);
        json list = json::array();
        for (const Item& it : page.items) {
            json j = itemJson(it);
            // Tells the page which buttons to show. The server still checks again on every action.
            const bool canClose = (user.role == "admin" || it.ownerId == user.id);
            j["canClose"] = canClose;
            if (it.type == "lost" || it.type == "found") {
                // Only the owner needs to know how many claims are waiting.
                j["pendingClaims"] = canClose ? claimRepo.countPending(it.id) : 0;
            }
            list.push_back(j);
        }
        reply(res, 200, json{{"items", list}, {"total", page.total}, {"limit", limit}, {"offset", offset}});
    });

    server.Post("/api/items", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        if (user.role != "admin" && user.emailVerifiedAt == 0) {
            return replyError(res, 403, "verify your email address before posting a notice");
        }
        json body;
        if (!parseBody(req, res, body)) return;

        Item item;
        item.type = str(body, "type");
        item.title = str(body, "title");
        item.description = str(body, "description");
        item.category = str(body, "category");
        item.location = str(body, "location");
        item.proof = str(body, "proof");
        item.contact = str(body, "contact");
        auto price = body.find("price");
        item.price = (price != body.end() && price->is_number()) ? price->get<double>() : 0.0;
        // The poster is whoever is logged in. Any "owner" sent by the browser is ignored.
        item.owner = user.name;
        item.ownerId = user.id;

        std::string err = validateItem(item);
        if (!err.empty()) return replyError(res, 400, err);
        reply(res, 201, itemJson(store.add(item)));
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
        // Also delete the image file from disk if it exists.
        std::optional<Item> it = store.find(id);
        Store::Result r = store.remove(id, user.id, user.role == "admin");
        if (r == Store::Result::Ok && it && !it->imageMime.empty()) {
            const std::string ext = extForMime(it->imageMime);
            const std::string imgPath = imagesDir + "/" + std::to_string(id) + ext;
            std::filesystem::remove(imgPath);
        }
        sendResult(res, r);
    });

    // ---------- Phase 8: item image upload and serving ----------
    // Upload: accepts the raw image bytes as the request body (Content-Type is
    // the MIME type). Magic-byte sniffing confirms the type, ignoring the header.
    server.Post(R"(/api/items/(\d+)/image)", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        const int id = parseId(req.matches[1]);
        std::optional<Item> item = store.find(id);
        if (!item) return replyError(res, 404, "not found");
        if (user.role != "admin" && item->ownerId != user.id) {
            return replyError(res, 403, "only the owner or an admin can upload a photo");
        }
        if (req.body.empty() || req.body.size() > 4 * 1024 * 1024) {
            return replyError(res, 400, "image must be between 1 byte and 4 MB");
        }
        const auto sniffed = sniffImage(req.body);
        if (!sniffed) return replyError(res, 400, "file must be a JPEG, PNG, GIF or WebP image");

        // Remove the old image file if one existed.
        if (!item->imageMime.empty()) {
            const std::string oldExt = extForMime(item->imageMime);
            std::filesystem::remove(imagesDir + "/" + std::to_string(id) + oldExt);
        }

        // Write to disk first (durability), then update the database and memory.
        const std::string imgPath = imagesDir + "/" + std::to_string(id) + sniffed->ext;
        {
            std::ofstream f(imgPath, std::ios::binary);
            f.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
            if (!f) return replyError(res, 500, "could not save the image");
        }
        itemRepo.setImageMime(id, sniffed->mime);
        store.setImageMime(id, sniffed->mime);
        reply(res, 200, json{{"ok", true}, {"mime", sniffed->mime}});
    });

    // Serve: reads the file from disk and streams it back.
    server.Get(R"(/api/items/(\d+)/image)", [&](const httplib::Request& req, httplib::Response& res) {
        const int id = parseId(req.matches[1]);
        std::optional<Item> item = store.find(id);
        if (!item || item->imageMime.empty()) {
            replyError(res, 404, "no image for this notice");
            return;
        }
        const std::string ext = extForMime(item->imageMime);
        const std::string imgPath = imagesDir + "/" + std::to_string(id) + ext;
        std::ifstream f(imgPath, std::ios::binary);
        if (!f) { replyError(res, 404, "image file not found"); return; }
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        res.status = 200;
        res.set_header("Cache-Control", "public, max-age=86400");
        res.set_content(bytes, item->imageMime);
    });

    // ---------- lost & found claims (Phase 6) ----------
    // Only the owner or an admin sees the claims on a notice.
    server.Get(R"(/api/items/(\d+)/claims)", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        const int itemId = parseId(req.matches[1]);
        std::optional<Item> item = store.find(itemId);
        if (!item) return replyError(res, 404, "not found");
        if (user.role != "admin" && item->ownerId != user.id) {
            return replyError(res, 403, "only the person who posted this or an admin can see its claims");
        }
        json list = json::array();
        for (const Claim& claim : claimRepo.listForItem(itemId)) {
            list.push_back(json{{"id", claim.id},
                                {"claimantName", claim.claimantName},
                                {"answer", claim.answer},
                                {"message", claim.message},
                                {"contact", claim.contact},
                                {"status", claim.status},
                                {"createdAt", claim.createdAt}});
        }
        reply(res, 200, list);
    });

    // A logged-in, verified user claims a lost/found notice.
    server.Post(R"(/api/items/(\d+)/claims)", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        if (user.emailVerifiedAt == 0) return replyError(res, 403, "verify your email address first");
        const int itemId = parseId(req.matches[1]);
        std::optional<Item> item = store.find(itemId);
        if (!item) return replyError(res, 404, "not found");
        if (item->type != "lost" && item->type != "found") {
            return replyError(res, 400, "claims are only possible on lost and found notices");
        }
        if (item->status != "open") return replyError(res, 400, "this notice is already closed");
        if (item->ownerId == user.id) return replyError(res, 400, "you cannot claim your own notice");

        json body;
        if (!parseBody(req, res, body)) return;
        const std::string answer = trim(str(body, "answer"));
        const std::string message = trim(str(body, "message"));
        const std::string contact = trim(str(body, "contact"));
        if (answer.empty() || utf8Length(answer) > 200) return replyError(res, 400, "the answer must be 1 to 200 characters");
        if (contact.empty() || utf8Length(contact) > 100) return replyError(res, 400, "the contact must be 1 to 100 characters");
        if (utf8Length(message) > 200) return replyError(res, 400, "the message must be at most 200 characters");
        if (claimRepo.exists(itemId, user.id)) return replyError(res, 400, "you already sent a claim for this notice");
        if (rateLimited("claim-ip:" + clientIp(req, trustProxy), 20, 3600)) {
            return replyError(res, 429, "too many claims from this network, try later");
        }
        const int claimId = claimRepo.create(itemId, user.id, answer, message, contact);
        if (claimId < 0) return replyError(res, 400, "you already sent a claim for this notice");

        // Notify the notice owner that someone has submitted a claim.
        notifRepo.create(item->ownerId, "claim", itemId,
                         user.name + " submitted a claim for \"" + item->title + "\"");

        reply(res, 201, json{{"id", claimId}});
    });

    // The owner (or admin) approves or rejects one claim.
    server.Post(R"(/api/items/(\d+)/claims/(\d+)/decide)", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        const int itemId = parseId(req.matches[1]);
        const int claimId = parseId(req.matches[2]);
        std::optional<Item> item = store.find(itemId);
        if (!item) return replyError(res, 404, "not found");
        if (user.role != "admin" && item->ownerId != user.id) {
            return replyError(res, 403, "only the person who posted this or an admin can decide claims");
        }
        json body;
        if (!parseBody(req, res, body)) return;
        const auto approve = body.find("approve");
        if (approve == body.end() || !approve->is_boolean()) {
            return replyError(res, 400, "approve must be true or false");
        }
        const bool approved = approve->get<bool>();

        // Find the claimant id before deciding (needed for notifications).
        std::vector<Claim> claims = claimRepo.listForItem(itemId);
        int claimantId = 0;
        for (const Claim& c : claims) {
            if (c.id == claimId) { claimantId = c.claimantId; break; }
        }

        DecideResult result = claimRepo.decide(itemId, claimId, user.id, approved, std::time(nullptr));
        if (result.status == DecideResult::Status::NotFound) {
            return replyError(res, 400, "this claim was already decided or does not exist");
        }
        if (approved) {
            store.markClosed(itemId);  // the transaction closed the database row
            // Notify the approved claimant.
            if (claimantId != 0) {
                notifRepo.create(claimantId, "approved", itemId,
                                 "Your claim for \"" + item->title + "\" was approved");
            }
            // Notify every claimant whose pending claim was auto-rejected.
            for (int rid : result.rejectedClaimantIds) {
                notifRepo.create(rid, "rejected", itemId,
                                 "Your claim for \"" + item->title + "\" was not approved");
            }
        } else {
            // Manual rejection by the owner.
            if (claimantId != 0) {
                notifRepo.create(claimantId, "rejected", itemId,
                                 "Your claim for \"" + item->title + "\" was not approved");
            }
        }
        reply(res, 200, json{{"ok", true}, {"approved", approved}});
    });

    // The claimant's own view: status, and the owner's contact once approved.
    server.Get("/api/claims/mine", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        json list = json::array();
        for (const MyClaim& claim : claimRepo.listForClaimant(user.id)) {
            list.push_back(json{{"id", claim.id},
                                {"itemId", claim.itemId},
                                {"itemTitle", claim.itemTitle},
                                {"itemType", claim.itemType},
                                {"status", claim.status},
                                {"ownerName", claim.ownerName},
                                {"ownerContact", claim.ownerContact},
                                {"createdAt", claim.createdAt}});
        }
        reply(res, 200, list);
    });

    // ---------- Phase 8: notifications ----------
    // Returns up to 30 recent notifications for the logged-in user.
    server.Get("/api/notifications", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        json list = json::array();
        for (const Notification& n : notifRepo.listFor(user.id, 30)) {
            list.push_back(json{{"id", n.id}, {"kind", n.kind}, {"itemId", n.itemId},
                                {"text", n.text}, {"createdAt", n.createdAt}, {"read", n.read}});
        }
        reply(res, 200, list);
    });

    // Marks all notifications as read.
    server.Post("/api/notifications/read", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireUser(req, res, user)) return;
        notifRepo.markAllRead(user.id, static_cast<long long>(std::time(nullptr)));
        reply(res, 200, json{{"ok", true}});
    });

    // ---------- Phase 8: admin dashboard ----------
    // Stats overview: user count, item counts, session count, claim counts.
    server.Get("/api/admin/stats", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireAdmin(req, res, user)) return;
        const long long now = static_cast<long long>(std::time(nullptr));
        const long long dayAgo = now - 24 * 60 * 60;
        reply(res, 200, json{
            {"users",         userRepo.count()},
            {"newUsersDay",   userRepo.countSince(dayAgo)},
            {"openItems",     itemRepo.countOpen()},
            {"closedItems",   itemRepo.countClosed()},
            {"sellItems",     itemRepo.countByType("sell")},
            {"lendItems",     itemRepo.countByType("lend")},
            {"lostItems",     itemRepo.countByType("lost")},
            {"foundItems",    itemRepo.countByType("found")},
            {"activeSessions",sessionRepo.countActive(now)},
            {"pendingClaims", claimRepo.countByStatus("pending")},
            {"approvedClaims",claimRepo.countByStatus("approved")},
            {"rejectedClaims",claimRepo.countByStatus("rejected")}
        });
    });

    // List all users (admin only).
    server.Get("/api/admin/users", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireAdmin(req, res, user)) return;
        json list = json::array();
        for (const User& u : userRepo.adminList()) {
            list.push_back(json{{"id", u.id}, {"name", u.name}, {"email", u.email},
                                {"role", u.role}, {"emailVerified", u.emailVerifiedAt != 0},
                                {"banned", u.bannedAt != 0}, {"createdAt", u.createdAt}});
        }
        reply(res, 200, list);
    });

    // Ban or unban a user (admin only). An admin cannot ban themselves.
    server.Post(R"(/api/admin/users/(\d+)/ban)", [&](const httplib::Request& req, httplib::Response& res) {
        User admin;
        if (!requireAdmin(req, res, admin)) return;
        const int targetId = parseId(req.matches[1]);
        if (targetId < 0) return replyError(res, 404, "not found");
        if (targetId == admin.id) return replyError(res, 400, "you cannot ban yourself");
        json body;
        if (!parseBody(req, res, body)) return;
        const auto banFlag = body.find("ban");
        if (banFlag == body.end() || !banFlag->is_boolean()) {
            return replyError(res, 400, "ban must be true or false");
        }
        const bool doBan = banFlag->get<bool>();
        std::optional<User> target = userRepo.findById(targetId);
        if (!target) return replyError(res, 404, "user not found");
        if (doBan) {
            userRepo.ban(targetId, static_cast<long long>(std::time(nullptr)));
            // Invalidate the target's sessions immediately: the next request from
            // any of their devices will be refused.
            sessionRepo.removeAllForUser(targetId);
        } else {
            userRepo.unban(targetId);
        }
        reply(res, 200, json{{"ok", true}, {"banned", doBan}});
    });

    // List all claims across all notices (admin overview).
    server.Get("/api/admin/claims", [&](const httplib::Request& req, httplib::Response& res) {
        User user;
        if (!requireAdmin(req, res, user)) return;
        const int limit = 50;
        int offset = 0;
        if (req.has_param("offset")) {
            try { offset = std::stoi(req.get_param_value("offset")); } catch (...) {}
        }
        json list = json::array();
        for (const AdminClaim& ac : claimRepo.listAll(limit, offset)) {
            list.push_back(json{{"id", ac.id}, {"itemId", ac.itemId}, {"itemTitle", ac.itemTitle},
                                {"claimantId", ac.claimantId}, {"claimantName", ac.claimantName},
                                {"status", ac.status}, {"createdAt", ac.createdAt}});
        }
        reply(res, 200, list);
    });

    server.set_mount_point("/", env("CC_FRONTEND_DIR", "../frontend"));

    std::cout << "CampusConnect running on http://localhost:" << port << std::endl;
    std::cout << "Database: " << dataDir << "/campus.db" << std::endl;
    server.listen("0.0.0.0", port);
}
