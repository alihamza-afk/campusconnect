// ---------------------------------------------------------------------------
// mailer.cpp - console mailer + Brevo mailer.
//
// The Brevo implementation is a single HTTPS POST with an "api-key" header:
//   POST https://api.brevo.com/v3/smtp/email
//   { sender, to, subject, textContent }
// libcurl is already on macOS and on every Linux image, so this adds no SDK.
// ---------------------------------------------------------------------------
#include "mailer.h"

#include <curl/curl.h>

#include <iostream>
#include <utility>

#include "json.hpp"

namespace {

// libcurl needs a write callback even when we do not care about the response body.
size_t discardBody(char* data, size_t size, size_t count, void* userdata) {
    (void)data;
    (void)userdata;
    return size * count;
}

}  // namespace

// ---------------------------------------------------------------------------
// ConsoleMailer
// ---------------------------------------------------------------------------
bool ConsoleMailer::send(const std::string& toEmail, const std::string& toName,
                         const std::string& subject, const std::string& body) {
    // std::flush matters: the link must be visible before the HTTP response is
    // sent, otherwise a test or a demo cannot read it from the server log.
    std::cout << "\n---------- EMAIL (console mailer) ----------\n"
              << "To:      " << toName << " <" << toEmail << ">\n"
              << "Subject: " << subject << "\n\n"
              << body << "\n"
              << "--------------------------------------------\n"
              << std::flush;
    return true;
}

// ---------------------------------------------------------------------------
// BrevoMailer
// ---------------------------------------------------------------------------
BrevoMailer::BrevoMailer(std::string apiKey, std::string fromEmail, std::string fromName)
    : apiKey_(std::move(apiKey)), fromEmail_(std::move(fromEmail)), fromName_(std::move(fromName)) {}

bool BrevoMailer::send(const std::string& toEmail, const std::string& toName,
                       const std::string& subject, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "mailer: could not initialise libcurl" << std::endl;
        return false;
    }

    nlohmann::json payload = {
        {"sender", {{"email", fromEmail_}, {"name", fromName_}}},
        {"to", nlohmann::json::array({{{"email", toEmail}, {"name", toName}}})},
        {"subject", subject},
        {"textContent", body},
    };
    const std::string requestBody = payload.dump();

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("api-key: " + apiKey_).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.brevo.com/v3/smtp/email");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(requestBody.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    const CURLcode result = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        std::cerr << "mailer: sending failed: " << curl_easy_strerror(result) << std::endl;
        return false;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        std::cerr << "mailer: Brevo answered HTTP " << httpStatus << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
std::unique_ptr<Mailer> makeMailer(const std::string& mode, const std::string& apiKey,
                                   const std::string& fromEmail, const std::string& fromName) {
    if (mode == "brevo") {
        if (apiKey.empty()) {
            std::cerr << "mailer: CC_MAIL_MODE=brevo but CC_BREVO_API_KEY is empty; "
                         "falling back to the console mailer" << std::endl;
            return std::make_unique<ConsoleMailer>();
        }
        // Called once at start-up, before any request thread exists.
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return std::make_unique<BrevoMailer>(apiKey, fromEmail, fromName);
    }
    if (!mode.empty() && mode != "console") {
        std::cerr << "mailer: unknown CC_MAIL_MODE '" << mode << "', using console" << std::endl;
    }
    return std::make_unique<ConsoleMailer>();
}
