// The command guard: the revision strings views pass, and each refusal checkCommand gives before a
// result command can change work.
#include "TestHarness.h"
#include "TestSession.h"
#include "app/CommandGuard.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/SyncFaults.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <limits>
#include <memory>
#include <optional>
#include <variant>

using namespace branch_truce;
using test_support::TestSession;

namespace {
// What checkCommand returned: the target's revision, or the refusal.
std::variant<RevisionId, Transition> outcome(const std::variant<CommandTarget, Transition> &checked)
{
    if (const auto *target = std::get_if<CommandTarget>(&checked)) return target->revision;
    return std::get<Transition>(checked);
}

// A session holding the supported "choices" file and an outside-app file, with "choices" at
// revision 2 after one edit.
struct Guarded {
    Guarded()
    {
        CHECK(directory.isValid());
        CHECK(store.appendFile(test_support::choiceFile()));
        CHECK(store.appendFile(FileSession("outside", "diagram.png", {},
                                           {.support = Support::OutsideApp, .supportDetail = "image content"})));
        CHECK(store.editResult("choices", 1, {{0, 0}, "edited\n"}) == Transition::Applied);
        CHECK(store.file("choices")->current().id == 2);
    }
    std::variant<RevisionId, Transition> check(const QString &fileId, const QString &revision, CommandCheck kind,
                                               bool composing = false, const QString &selected = "choices") const
    {
        return outcome(checkCommand(store, selected, composing, fileId, revision, kind));
    }
    QTemporaryDir directory;
    std::shared_ptr<test_support::SyncFaults> faults = std::make_shared<test_support::SyncFaults>();
    TestSession store{directory.path() + "/recovery", faults};
};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("CommandGuardTests");

    struct Revision {
        const char *text;
        std::optional<RevisionId> parsed;
    };
    for (const auto &[text, parsed] :
         std::initializer_list<Revision>{{"1", 1},
                                         {"42", 42},
                                         {"18446744073709551615", std::numeric_limits<RevisionId>::max()},
                                         {"0", std::nullopt},
                                         {"01", std::nullopt},
                                         {"-1", std::nullopt},
                                         {"+1", std::nullopt},
                                         {" 1", std::nullopt},
                                         {"1 ", std::nullopt},
                                         {"1.0", std::nullopt},
                                         {"0x1", std::nullopt},
                                         {"18446744073709551616", std::nullopt},
                                         {"", std::nullopt},
                                         {"one", std::nullopt}}) {
        suite.run(std::string("a revision string parses only in its canonical decimal form: \"") + text + '"',
                  [=] { CHECK_EQ(parseRevisionId(QString::fromLatin1(text)), parsed); });
    }

    suite.run("an unknown file, an unparsable revision and another selected file are stale", [] {
        const Guarded session;
        using Result = std::variant<RevisionId, Transition>;
        for (const auto kind :
             {CommandCheck::Selected, CommandCheck::Current, CommandCheck::Edit, CommandCheck::CurrentSupported}) {
            SCOPE("check " + std::to_string(static_cast<int>(kind)));
            CHECK(session.check("missing", "2", kind, false, "missing") == Result{Transition::StaleRevision});
            CHECK(session.check("choices", "02", kind) == Result{Transition::StaleRevision});
            CHECK(session.check("choices", "2", kind, false, "outside") == Result{Transition::StaleRevision});
        }
    });
    suite.run("an earlier revision is stale for every command but a preview, which checks it itself", [] {
        const Guarded session;
        using Result = std::variant<RevisionId, Transition>;
        CHECK(session.check("choices", "1", CommandCheck::Selected) == Result{RevisionId{1}});
        for (const auto kind : {CommandCheck::Current, CommandCheck::Edit, CommandCheck::CurrentSupported})
            CHECK(session.check("choices", "1", kind) == Result{Transition::StaleRevision});
        CHECK(session.check("choices", "2", CommandCheck::Current) == Result{RevisionId{2}});
    });
    suite.run("while an input method composes only the native edit it produces is admitted", [] {
        const Guarded session;
        using Result = std::variant<RevisionId, Transition>;
        CHECK(session.check("choices", "2", CommandCheck::Edit, true) == Result{RevisionId{2}});
        for (const auto kind : {CommandCheck::Selected, CommandCheck::Current, CommandCheck::CurrentSupported})
            CHECK(session.check("choices", "2", kind, true) == Result{Transition::Blocked});
    });
    suite.run("Save, Mark resolved and discards refuse a file the app cannot edit", [] {
        const Guarded session;
        using Result = std::variant<RevisionId, Transition>;
        CHECK(session.check("outside", "1", CommandCheck::CurrentSupported, false, "outside")
              == Result{Transition::Blocked});
        CHECK(session.check("outside", "1", CommandCheck::Current, false, "outside") == Result{RevisionId{1}});
    });
    suite.run("a recovery error on the file blocks every command until it is settled", [] {
        Guarded session;
        using Result = std::variant<RevisionId, Transition>;
        session.store.recordRecoveryFailure("choices", ProblemCode::RecoveryStoreUnavailable);
        for (const auto kind :
             {CommandCheck::Selected, CommandCheck::Current, CommandCheck::Edit, CommandCheck::CurrentSupported})
            CHECK(session.check("choices", "2", kind) == Result{Transition::Blocked});
        CHECK(session.store.settleRecoveryFailure("choices", ProblemCode::RecoveryStoreUnavailable));
        CHECK(session.check("choices", "2", CommandCheck::Current) == Result{RevisionId{2}});
    });
    suite.run("pending recovery input on any file blocks every command until it is retried", [] {
        Guarded session;
        using Result = std::variant<RevisionId, Transition>;
        {
            const test_support::ArmedFault fault(*session.faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(session.store.editResult("choices", 2, {{0, 0}, "pending\n"}) == Transition::Blocked);
        }
        CHECK(session.store.hasPendingRecovery());
        CHECK(session.check("outside", "1", CommandCheck::Current, false, "outside") == Result{Transition::Blocked});
        CHECK(session.check("choices", "2", CommandCheck::Edit) == Result{Transition::Blocked});
        CHECK(session.store.retryPendingRecovery("choices"));
        CHECK(session.check("choices", "3", CommandCheck::Current) == Result{RevisionId{3}});
    });
    return suite.report();
}
