#pragma once

// A Git runner for the code under test: real Git, except that `intercept` sees each request first.
// It may change the repository and return nothing, so real Git runs the request, or return a
// result that stands for Git hanging past its deadline, failing or printing garbage.
//
// It keeps the GitRunner contract and GitAdapter's cancellation: start() refuses while busy, every
// accepted request finishes exactly once from the event loop, a cancel() before the answer arrives
// ends a request whose mutation was not attempted as Cancelled and keeps any other outcome, and
// destruction emits nothing.

#include "git/GitAdapter.h"

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace test_support {

class ScriptedGit final : public branch_truce::git::GitRunner {
public:
    using Intercept = std::function<std::optional<branch_truce::git::Result>(const branch_truce::git::Request &)>;

    ScriptedGit(const branch_truce::git::GitProgram &program, Intercept intercept, QObject *parent = nullptr)
        : GitRunner(parent), real_(program, this), intercept_(std::move(intercept))
    {
        connect(&real_, &branch_truce::git::GitRunner::finished, this,
                [this](const branch_truce::git::Result &result) { Q_EMIT finished(result); });
    }

    // Runners for a session or service that all share one intercept.
    static branch_truce::git::GitRunners runners(branch_truce::git::GitProgram program, Intercept intercept)
    {
        return [program = std::move(program),
                intercept = std::move(intercept)](QObject *parent) -> std::unique_ptr<branch_truce::git::GitRunner> {
            return std::make_unique<ScriptedGit>(program, intercept, parent);
        };
    }

    bool start(const branch_truce::git::Request &request) override
    {
        if (busy()) return false;
        auto scripted = intercept_(request);
        if (!scripted) return real_.start(request);
        scripted->request = request;
        pending_ = std::move(scripted);
        // Answered from the event loop, never inside start(), as a real Git process would.
        QTimer::singleShot(0, this, [this] {
            if (pending_) Q_EMIT finished(*std::exchange(pending_, std::nullopt));
        });
        return true;
    }
    void cancel() override
    {
        if (!pending_) return real_.cancel();
        pending_->cancellationRequested = true;
        if (pending_->mutation == branch_truce::git::Mutation::NotAttempted) {
            pending_->outcome = branch_truce::git::Outcome::Cancelled;
            pending_->problem = {branch_truce::ProblemCode::GitCancelled};
        }
    }
    [[nodiscard]] bool busy() const override { return pending_.has_value() || real_.busy(); }

private:
    branch_truce::git::GitAdapter real_;
    Intercept intercept_;
    std::optional<branch_truce::git::Result> pending_;
};

} // namespace test_support
