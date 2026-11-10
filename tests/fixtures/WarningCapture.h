#pragma once

// Counts the Qt messages that match a predicate while it lives, and passes every message on to the
// handler that was installed before it. One capture is active at a time.

#include <QString>
#include <QtGlobal>

#include <atomic>
#include <cstdio>
#include <functional>
#include <utility>

namespace test_support {

class WarningCapture {
public:
    explicit WarningCapture(std::function<bool(const QString &)> matches) : matches_(std::move(matches))
    {
        active() = this;
        previous_ = qInstallMessageHandler(&WarningCapture::handle);
    }
    ~WarningCapture()
    {
        qInstallMessageHandler(previous_);
        active() = nullptr;
    }
    WarningCapture(const WarningCapture &) = delete;
    WarningCapture &operator=(const WarningCapture &) = delete;

    [[nodiscard]] int count() const { return count_.load(); }

private:
    static WarningCapture *&active()
    {
        static WarningCapture *current = nullptr;
        return current;
    }
    static void handle(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        auto *capture = active();
        if (!capture) return;
        if (capture->matches_(message)) ++capture->count_;
        if (capture->previous_)
            capture->previous_(type, context, message);
        else
            std::fprintf(stderr, "%s\n", qPrintable(message));
    }

    std::function<bool(const QString &)> matches_;
    QtMessageHandler previous_ = nullptr;
    std::atomic_int count_{0};
};

} // namespace test_support
