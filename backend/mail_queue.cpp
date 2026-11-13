#include "mail_queue.h"

#include <utility>

MailQueue::MailQueue(Mailer& backendMailer)
    : rawBackend_(&backendMailer) {
    start();
}

MailQueue::MailQueue(Mailer* backendMailer)
    : rawBackend_(backendMailer) {
    start();
}

MailQueue::MailQueue(std::unique_ptr<Mailer> backendMailer)
    : ownedBackend_(std::move(backendMailer)),
      rawBackend_(ownedBackend_.get()) {
    start();
}

MailQueue::~MailQueue() {
    stop();
}

Mailer* MailQueue::getBackend() const {
    if (ownedBackend_) return ownedBackend_.get();
    return rawBackend_;
}

bool MailQueue::send(const std::string& toEmail,
                     const std::string& toName,
                     const std::string& subject,
                     const std::string& body) {
    {
        std::lock_guard<std::mutex> lock(mu_);

        if (stopRequested_) return false;

        queue_.push_back({toEmail, toName, subject, body});
    }

    cv_.notify_one();
    return true;
}

void MailQueue::push(const MailTask& task) {
    {
        std::lock_guard<std::mutex> lock(mu_);

        if (stopRequested_) return;

        queue_.push_back(task);
    }

    cv_.notify_one();
}

void MailQueue::start() {
    std::lock_guard<std::mutex> lock(mu_);

    if (!worker_.joinable()) {
        stopRequested_ = false;
        worker_ = std::thread(&MailQueue::workerLoop, this);
    }
}

void MailQueue::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopRequested_ = true;
    }

    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
}

size_t MailQueue::pendingCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size() + activeCount_;
}

size_t MailQueue::pending() const {
    return pendingCount();
}

bool MailQueue::waitIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);

    return idleCv_.wait_for(lock, timeout, [this] {
        return queue_.empty() && activeCount_ == 0;
    });
}

void MailQueue::workerLoop() {
    while (true) {
        MailTask task;

        {
            std::unique_lock<std::mutex> lock(mu_);

            cv_.wait(lock, [this] {
                return stopRequested_ || !queue_.empty();
            });

            if (stopRequested_ && queue_.empty()) {
                break;
            }

            if (queue_.empty()) {
                continue;
            }

            task = std::move(queue_.front());
            queue_.pop_front();
            ++activeCount_;
        }

        Mailer* backend = getBackend();

        try {
            if (backend) {
                backend->send(task.to, task.name, task.subject, task.body);
            }
        } catch (...) {
            // A failed delivery must not terminate the queue worker.
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            --activeCount_;

            if (queue_.empty() && activeCount_ == 0) {
                idleCv_.notify_all();
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);

        if (queue_.empty() && activeCount_ == 0) {
            idleCv_.notify_all();
        }
    }
}
