#include "email_service.h"

#include "secrets.h"
#include "password.h"

#include <ctime>
#include <utility>

EmailService::EmailService(EmailTokenRepo& tokens, UserRepo& users,
                           SessionRepo& sessions, Mailer& mailer,
                           std::string baseUrl)
    : tokens_(tokens),
      users_(users),
      sessions_(sessions),
      mailer_(mailer),
      baseUrl_(std::move(baseUrl)) {}

EmailService::SendResult
EmailService::sendVerification(int userId, const std::string& email) {
    auto user = users_.findById(userId);
    if (!user) return SendResult::Failed;

    const long long now = static_cast<long long>(std::time(nullptr));

    if (tokens_.recentlyIssued(userId, "verify", now - 60)) {
        return SendResult::TooSoon;
    }

    const std::string rawToken = randomToken();
    const std::string tokenHash = hashToken(rawToken);

    tokens_.create(userId, "verify", tokenHash, now + 24 * 60 * 60);

    const std::string link = baseUrl_ + "/?verify=" + rawToken;
    const std::string body =
        "Hi " + user->name +
        ",\n\nConfirm your CampusConnect email address by opening this link:\n\n" +
        link + "\n\nThe link works once and expires in 24 hours.\n";

    if (!mailer_.send(email, user->name,
                      "Verify your CampusConnect email", body)) {
        // Do not leave a token behind when no verification email was queued.
        // Otherwise the resend cooldown can block a user who received nothing.
        tokens_.deleteToken(tokenHash);
        return SendResult::Failed;
    }

    return SendResult::Sent;
}

EmailService::SendResult
EmailService::sendVerification(const User& user) {
    return sendVerification(user.id, user.email);
}

EmailService::VerifyResult
EmailService::verifyEmail(const std::string& rawToken) {
    const std::string tokenHash = hashToken(rawToken);
    const long long now = static_cast<long long>(std::time(nullptr));

    auto userId = tokens_.findUserId(tokenHash, "verify", now);
    if (!userId) return VerifyResult::Invalid;

    if (!tokens_.consume(tokenHash, now)) {
        return VerifyResult::Invalid;
    }

    users_.markVerified(*userId, now);
    return VerifyResult::Ok;
}

EmailService::VerifyResult
EmailService::verify(const std::string& rawToken) {
    return verifyEmail(rawToken);
}

EmailService::SendResult
EmailService::sendPasswordReset(const std::string& email) {
    auto user = users_.findByEmail(email);

    // Do not reveal whether an email address has an account.
    if (!user) return SendResult::Sent;

    const long long now = static_cast<long long>(std::time(nullptr));

    if (tokens_.recentlyIssued(user->id, "reset", now - 60)) {
        return SendResult::TooSoon;
    }

    const std::string rawToken = randomToken();
    const std::string tokenHash = hashToken(rawToken);

    tokens_.create(user->id, "reset", tokenHash, now + 60 * 60);

    const std::string link = baseUrl_ + "/?reset=" + rawToken;
    const std::string body =
        "Hi " + user->name +
        ",\n\nReset your CampusConnect password by opening this link:\n\n" +
        link + "\n\nThe link expires in 1 hour.\n";

    if (!mailer_.send(user->email, user->name,
                      "Reset your CampusConnect password", body)) {
        // A failed delivery must not leave a reset token or trigger cooldown.
        tokens_.deleteToken(tokenHash);
        return SendResult::Failed;
    }

    return SendResult::Sent;
}

EmailService::SendResult
EmailService::sendPasswordReset(const User& user) {
    return sendPasswordReset(user.email);
}

EmailService::ResetResult
EmailService::resetPassword(const std::string& rawToken,
                            const std::string& newPassword) {
    if (newPassword.size() < 8) {
        return ResetResult::WeakPassword;
    }

    const std::string tokenHash = hashToken(rawToken);
    const long long now = static_cast<long long>(std::time(nullptr));

    auto userId = tokens_.findUserId(tokenHash, "reset", now);
    if (!userId) return ResetResult::InvalidToken;

    if (!tokens_.consume(tokenHash, now)) {
        return ResetResult::InvalidToken;
    }

    const std::string newHash = hashPassword(newPassword);
    users_.setPasswordHash(*userId, newHash);
    sessions_.removeAllForUser(*userId);

    return ResetResult::Ok;
}
