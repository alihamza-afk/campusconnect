#ifndef EMAIL_SERVICE_H
#define EMAIL_SERVICE_H

#include "email_token_repo.h"
#include "user_repo.h"
#include "session_repo.h"
#include "mailer.h"

#include <string>

class EmailService {
public:
    enum class SendResult { Sent, TooSoon, Failed };
    enum class VerifyResult { Ok, Invalid, Expired };
    enum class ResetResult { Ok, InvalidToken, WeakPassword };

    EmailService(EmailTokenRepo& tokens, UserRepo& users,
                 SessionRepo& sessions, Mailer& mailer,
                 std::string baseUrl);

    SendResult sendVerification(int userId, const std::string& email);
    SendResult sendVerification(const User& user);

    VerifyResult verifyEmail(const std::string& rawToken);
    VerifyResult verify(const std::string& rawToken);

    SendResult sendPasswordReset(const std::string& email);
    SendResult sendPasswordReset(const User& user);

    ResetResult resetPassword(const std::string& rawToken,
                              const std::string& newPassword);

private:
    EmailTokenRepo& tokens_;
    UserRepo& users_;
    SessionRepo& sessions_;
    Mailer& mailer_;
    std::string baseUrl_;
};

#endif
