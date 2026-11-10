// Finite admission for visual revision computations on the application's existing worker pool.
#pragma once
#include "core/Transition.h"
#include "core/internal/WorkAdmission.h"
#include "storage/OffThread.h"
#include <QPointer>
#include <QThread>
#include <functional>
#include <variant>

namespace branch_truce {
class RevisionWorkers final : public QObject {
    Q_OBJECT
public:
    explicit RevisionWorkers(QThreadPool &pool) : pool_(pool) {}
    // An injected compute gate runs independently for preview and decoration.
    std::function<void(bool)> beforeCompute;
    detail::WorkAdmission admission{2, 1073741824};
    template <typename Capture, typename Done>
    bool submit(std::size_t bytes, QObject *owner, bool words, Capture capture, Done done)
    {
        using Work = std::invoke_result_t<Capture>;
        using Value = std::invoke_result_t<Work>;
        using Result = std::variant<Value, Transition>;
        Q_ASSERT(QThread::currentThread() == thread());
        QPointer<QObject> guarded(owner);
        if (!admission.take(bytes)) return false;
        const auto finish = [this, bytes, owner = std::move(guarded), done = std::move(done)](const Result &result) {
            QPointer<RevisionWorkers> self(this);
            admission.release(bytes);
            if (owner) done(result);
            if (self) Q_EMIT available();
        };
        try {
            auto work = capture();
            runOffThread(
                pool_, this,
                [work = std::move(work), gate = beforeCompute, words]() mutable -> Result {
                    if (gate) gate(words);
                    return work();
                },
                finish, [](std::exception_ptr error) noexcept -> Result { return computationFailure(error); });
        } catch (...) {
            const Result result = computationFailure(std::current_exception());
            QMetaObject::invokeMethod(this, [finish, result] { finish(result); }, Qt::QueuedConnection);
        }
        return true;
    }
Q_SIGNALS:
    void available();

private:
    QThreadPool &pool_;
};
}
