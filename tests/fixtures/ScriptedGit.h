#pragma once

// A Git runner that answers one kind of request itself and runs every other request with real Git,
// for a case that needs Git to hang past its deadline, fail or print garbage on one operation.
// It keeps the GitRunner contract: start() refuses while busy, every accepted request finishes
// exactly once from the event loop, cancel() ends a scripted request as Cancelled, and destruction
// emits nothing.

#include "git/GitAdapter.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <utility>

namespace test_support {

class ScriptedGitRunner final : public branch_truce::git::GitRunner {
public:
    using Answer = std::function<branch_truce::git::Result(const branch_truce::git::Request &)>;

    ScriptedGitRunner(branch_truce::git::Operation scripted, Answer answer, const QString &executable,
                      QObject *parent = nullptr)
        : GitRunner(parent),
          scripted_(scripted),
          answer_(std::move(answer)),
          real_(std::make_unique<branch_truce::git::GitAdapter>(executable, this))
    {
        connect(real_.get(), &branch_truce::git::GitRunner::finished, this,
                [this](const branch_truce::git::Result &result) { Q_EMIT finished(result); });
    }

    // Runners for a session: each answers `scripted` requests with `answer`.
    static branch_truce::git::GitRunners runners(branch_truce::git::Operation scripted, Answer answer,
                                                 QString executable)
    {
        return [=](QObject *parent) -> std::unique_ptr<branch_truce::git::GitRunner> {
            return std::make_unique<ScriptedGitRunner>(scripted, answer, executable, parent);
        };
    }

    bool start(const branch_truce::git::Request &request) override
    {
        if (busy()) return false;
        if (request.operation != scripted_) return real_->start(request);
        pending_ = std::make_unique<branch_truce::git::Request>(request);
        cancelled_ = false;
        // Answered from the event loop, never inside start(), as a real Git process would.
        QTimer::singleShot(0, this, [this] { answer(); });
        return true;
    }
    void cancel() override
    {
        if (pending_)
            cancelled_ = true;
        else
            real_->cancel();
    }
    [[nodiscard]] bool busy() const override { return pending_ != nullptr || real_->busy(); }

private:
    void answer()
    {
        if (!pending_) return;
        const auto request = *std::exchange(pending_, nullptr);
        auto result = answer_(request);
        result.request = request;
        if (cancelled_) {
            result.outcome = branch_truce::git::Outcome::Cancelled;
            result.cancellationRequested = true;
        }
        Q_EMIT finished(result);
    }

    branch_truce::git::Operation scripted_;
    Answer answer_;
    std::unique_ptr<branch_truce::git::GitAdapter> real_;
    std::unique_ptr<branch_truce::git::Request> pending_;
    bool cancelled_ = false;
};

} // namespace test_support
