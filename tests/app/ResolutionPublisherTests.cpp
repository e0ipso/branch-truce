// The resolution publisher alone, on a session store and coordinator without a window or a loaded
// repository: what start() refuses or asks before it writes, and how it records a write it cannot
// make. Mark resolved on real repositories is in MarkResolvedTests.cpp.
#include "TestHarness.h"
#include "TestSession.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/GitRepository.h"
#include "publication/ResolutionPublisher.h"
#include "session/SessionCoordinator.h"
#include "storage/RecoveryStore.h"
#include <QCoreApplication>
#include <QStringList>
#include <QTemporaryDir>

using namespace branch_truce;

namespace {
// One file in a session store, with the publisher a window would build over it.
struct Publishing {
    explicit Publishing(FileSession file = test_support::choiceFile())
    {
        CHECK(directory.isValid());
        CHECK(store.appendFile(std::move(file)));
        QObject::connect(&publisher, &ResolutionPublisher::changed, &publisher,
                         [this](const QString &message) { messages << message; });
    }
    [[nodiscard]] const FileSession &file() const { return *store.file("choices"); }
    [[nodiscard]] RevisionId revision() const { return file().current().id; }
    [[nodiscard]] qsizetype records() const
    {
        return storage::RecoveryStore(store.recoveryDirectory()).read().transactions.size();
    }

    QTemporaryDir directory;
    test_support::TestSession store{directory.path() + "/recovery"};
    const git::GitRunners git = git::gitAdapters({test_support::gitExecutable(), git::GitEnvironment::fromProcess()});
    QThreadPool workers;
    SessionCoordinator coordinator{store, git, workers};
    ResolutionPublisher publisher{store, coordinator, git};
    QStringList messages;
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("ResolutionPublisherTests");
    suite.run("an unknown file or a revision other than the current one is refused as stale", [] {
        Publishing session;
        const auto before = session.records();
        for (const auto &[id, revision] : {std::pair{std::string("missing"), RevisionId{1}},
                                           std::pair{std::string("choices"), session.revision() + 1}}) {
            SCOPE(id);
            const auto refused = session.publisher.start(id, revision, true);
            CHECK(refused.transition == Transition::StaleRevision);
            CHECK_EQ(refused.detail, QStringLiteral("The result changed. Mark resolved was not applied."));
        }
        CHECK(!session.file().publication().reviewed() && !session.file().publication().writeOutcome()
              && !session.publisher.busy());
        CHECK_EQ(session.records(), before);
    });
    suite.run("undecided choices ask for confirmation, and cancelling it leaves the file as it was", [] {
        Publishing session;
        CHECK(session.file().facts().undecided > 0);
        const auto before = session.records();
        const auto asked = session.publisher.start("choices", session.revision(), false);
        CHECK(asked.transition == Transition::ConfirmationRequired);
        CHECK(session.publisher.confirmationPending());
        CHECK_EQ(session.publisher.confirmationFileId(), QStringLiteral("choices"));
        CHECK_EQ(session.publisher.confirmationRevision(), QString::number(session.revision()));
        CHECK(!session.file().publication().reviewed() && !session.publisher.busy());
        session.publisher.cancelConfirmation();
        CHECK(!session.publisher.confirmationPending());
        CHECK_EQ(session.messages,
                 QStringList{QStringLiteral("Resolution cancelled. The text and choices are unchanged.")});
        CHECK(!session.file().publication().reviewed());
        CHECK_EQ(session.records(), before);
    });
    suite.run("conflict markers refuse Mark resolved and name the first marker line", [] {
        Publishing session(test_support::markedFile());
        CHECK(session.file().current().draft.unresolvedMarkers > 0);
        const auto refused = session.publisher.start("choices", session.revision(), true);
        CHECK(refused.transition == Transition::Blocked);
        CHECK(refused.markers.has_value() && refused.markers->first().has_value());
        const auto line = static_cast<qulonglong>(refused.markers->first()->line + 1);
        CHECK(refused.detail.startsWith(QStringLiteral("Mark resolved is blocked: ")));
        CHECK(
            refused.detail.endsWith(QStringLiteral("; first at line %1. The repository file is unchanged.").arg(line)));
        CHECK(!session.file().publication().reviewed() && !session.file().publication().writeOutcome()
              && !session.publisher.busy());
    });
    suite.run("without a captured launch destination the write fails and the file stays unresolved", [] {
        Publishing session;
        const auto revision = session.revision();
        const auto refused = session.publisher.start("choices", revision, true);
        CHECK(refused.transition == Transition::Blocked);
        const QString detail =
            QStringLiteral("Repository write failed. No launch destination is available, and the file is unresolved.");
        CHECK_EQ(refused.detail, detail);
        // The review and the Save before the write are kept; the write is recorded as failed.
        CHECK(session.file().publication().reviewed() && session.file().publication().checkpoint()
              && *session.file().publication().checkpoint() == revision);
        CHECK(session.file().publication().writeOutcome()
              && session.file().publication().writeOutcome()->outcome == Outcome::Failed);
        CHECK_EQ(QString::fromStdString(session.file().publication().writeOutcome()->detail), detail);
        CHECK(!session.file().publication().written() && !session.file().publication().staged()
              && !session.file().facts().resolved);
        CHECK(!session.publisher.busy() && !session.publisher.confirmationPending());
        CHECK_EQ(session.coordinator.pendingTasks(), 0);
    });
    return suite.report();
}
