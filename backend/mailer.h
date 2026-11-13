#pragma once
// ---------------------------------------------------------------------------
// mailer.h - "send an email" as an interface, with two implementations.
//
// Why an interface?
//   * The demo must work offline and deterministically, so the default
//     implementation just prints the message (including the verification link)
//     to the server console.
//   * Real delivery is then a configuration change, not a code change.
//   * Tests can plug in a capturing mailer and assert on the message without
//     touching the network.
//
// This is the classic C++ "program to an interface" example: EmailService only
// knows Mailer&, never ConsoleMailer or BrevoMailer.
// ---------------------------------------------------------------------------
#include <memory>
#include <string>

class Mailer {
public:
    virtual ~Mailer() = default;

    // Returns true when the message was handed off (printed, or accepted by the
    // API). A failed send must not break the request that caused it; the caller
    // logs it and the user can ask for another link.
    virtual bool send(const std::string& toEmail, const std::string& toName,
                      const std::string& subject, const std::string& body) = 0;
};

// Prints the message to the server console. Includes the clickable link.
class ConsoleMailer : public Mailer {
public:
    bool send(const std::string& toEmail, const std::string& toName,
              const std::string& subject, const std::string& body) override;
};

// Sends through Brevo's transactional email API (free plan: 300 emails/day).
// Uses libcurl for one HTTPS POST, so there is no SDK dependency.
class BrevoMailer : public Mailer {
    std::string apiKey_;
    std::string fromEmail_;
    std::string fromName_;

public:
    BrevoMailer(std::string apiKey, std::string fromEmail, std::string fromName);
    bool send(const std::string& toEmail, const std::string& toName,
              const std::string& subject, const std::string& body) override;
};

// Builds the mailer selected by CC_MAIL_MODE ("console" or "brevo").
// Falls back to the console mailer if Brevo is selected without an API key, so
// a misconfiguration cannot silently swallow verification links.
std::unique_ptr<Mailer> makeMailer(const std::string& mode, const std::string& apiKey,
                                   const std::string& fromEmail, const std::string& fromName);
