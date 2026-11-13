#ifndef MAIL_QUEUE_H
#define MAIL_QUEUE_H

#include "mailer.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class MailQueue : public Mailer {
public:
    struct MailTask {
        std::string to;
        std::string name;
        std::string subject;
        std::string body;
    };

    explicit MailQueue(Mailer& backendMailer);
    explicit MailQueue(Mailer* backendMailer);
    explicit MailQueue(std::unique_ptr<Mailer> backendMailer);

    ~MailQueue() override;

    MailQueue(const MailQueue&) = delete;
    MailQueue& operator=(const MailQueue&) = delete;

    bool send(const std::string& toEmail,
              const std::string& toName,
              const std::string& subject,
              const std::string& body) override;

    void push(const MailTask& task);

    void start();
    void stop();

    size_t pendingCount() const;
    size_t pending() const;

    bool waitIdle(std::chrono::milliseconds timeout);

private:
    void workerLoop();
    Mailer* getBackend() const;

    std::unique_ptr<Mailer> ownedBackend_;
    Mailer* rawBackend_ = nullptr;

    std::deque<MailTask> queue_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;

    std::thread worker_;

    size_t activeCount_ = 0;
    bool stopRequested_ = false;
};

#endif
