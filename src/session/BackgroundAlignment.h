// Background alignment retains finite reservations across session generations until workers terminate.
#pragma once
#include "core/RevisionId.h"
#include "core/Transition.h"
#include "core/internal/WorkAdmission.h"
#include <QFuture>
#include <QObject>
#include <QPromise>
#include <QThreadPool>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace branch_truce {
class CorrespondenceMap;
class SessionStore;

class BackgroundAlignment final : public QObject {
    Q_OBJECT
public:
    explicit BackgroundAlignment(SessionStore &store, QObject *parent = nullptr, detail::WorkAdmission limits = {});
    // Completes observers before the owned pool drains. Admitted work has no fake deadline.
    ~BackgroundAlignment() override;
    QFuture<Transition> waitFor(const std::string &id, RevisionId revision);
    [[nodiscard]] const detail::WorkAdmission &admission() const { return admission_; }
Q_SIGNALS:
    void finished(const QString &id, quint64 revision, branch_truce::Transition result);

private:
    void request(int row);
    void resume();
    void settle();
    struct Outcome {
        Transition state;
        std::shared_ptr<const CorrespondenceMap> map = {};
    };
    void aligned(const std::string &id, RevisionId revision, quint64 generation, std::size_t bytes, const Outcome &);
    struct Waiter {
        std::string id;
        RevisionId revision;
        std::shared_ptr<QPromise<Transition>> promise;
    };
    SessionStore &store_;
    detail::WorkAdmission admission_;
    // Old generations keep their slots and maps until their actual terminal delivery.
    std::map<std::pair<quint64, std::string>, RevisionId> running_;
    std::map<std::string, std::pair<RevisionId, Transition>> failed_;
    std::vector<Waiter> waiters_;
    quint64 generation_ = 0;
    QThreadPool workers_;
};
}
