// Every problem code has a sentence, survives encoding, and reads the same wherever it is shown.
#include "TestHarness.h"
#include "problems/Problem.h"
#include "problems/ProblemText.h"

#include <QCoreApplication>

using namespace branch_truce;

namespace {
// Every code after None, in order.
QList<ProblemCode> everyCode()
{
    QList<ProblemCode> codes;
    for (int value = 1; !problemName(static_cast<ProblemCode>(value)).isEmpty(); ++value)
        codes.push_back(static_cast<ProblemCode>(value));
    return codes;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("ProblemTextTests");
    suite.run("every code has a stable name, a sentence and an encoding that round-trips", [] {
        const auto codes = everyCode();
        CHECK_GT(codes.size(), 200);
        QStringList names;
        for (const auto code : codes) {
            const auto name = problemName(code);
            SCOPE(name.toStdString());
            CHECK(!names.contains(name));
            names.push_back(name);
            const Problem problem(code, {QStringLiteral("first"), QStringLiteral("second")});
            CHECK(!problem_text::text(problem).isEmpty());
            CHECK(Problem::decode(problem.encoded()) == problem);
        }
        CHECK(problemName(ProblemCode::None).isEmpty() && Problem{}.encoded().isEmpty());
    });
    suite.run("a wrapped problem keeps its cause through encoding and in its sentence", [] {
        const auto inner = Problem(ProblemCode::GitExitCode, {QStringLiteral("23")});
        const auto outer = Problem(ProblemCode::GitReinspectionFailed).wrapping(inner);
        CHECK(outer.involves(ProblemCode::GitExitCode) && !inner.involves(ProblemCode::GitReinspectionFailed));
        CHECK(Problem::decode(outer.encoded()) == outer);
        CHECK_EQ(problem_text::text(outer),
                 QStringLiteral("Git exited with code 23; Index reinspection failed; mutation remains uncertain"));
    });
    suite.run("outside-app sentences carry their lead once, also when one wraps another", [] {
        const auto decoder = Problem(ProblemCode::TextControlCharacters);
        CHECK_EQ(problem_text::text(decoder), QStringLiteral("Outside app: binary/control characters are unsupported"));
        const auto stage =
            Problem(ProblemCode::ClassificationStageNotEditable, {QStringLiteral("1")}).wrapping(decoder);
        CHECK_EQ(problem_text::text(stage),
                 QStringLiteral("Outside app: stage 1 cannot be edited: binary/control characters are unsupported"));
        const auto operation =
            Problem(ProblemCode::ClassificationUnsupportedOperation).wrapping(ProblemCode::DiscoveryApplyMailbox);
        CHECK_EQ(problem_text::text(operation),
                 QStringLiteral("Outside app: apply-mailbox (git am) is not a supported rebase"));
        // Git's own words after an outside-app problem do not repeat the lead either.
        const auto filter = Problem(ProblemCode::GitFailedWithOutput, {QStringLiteral("fatal: no")})
                                .wrapping(Problem(ProblemCode::GitFilterPath));
        CHECK_EQ(problem_text::text(filter),
                 QStringLiteral("Outside app: filter attributes do not match the requested path: fatal: no"));
        CHECK_EQ(problem_text::text(
                     Problem(ProblemCode::ClassificationStageNotEditable, {QStringLiteral("2")}).wrapping(filter)),
                 QStringLiteral("Outside app: stage 2 cannot be edited: filter attributes do not match the requested "
                                "path: fatal: no"));
    });
    suite.run("a recovery problem reads the store problem it wraps", [] {
        const auto store = Problem(ProblemCode::RecoveryCommitFailed);
        const auto rejected = Problem(ProblemCode::RecoverySessionRejected, {QStringLiteral("s1")}).wrapping(store);
        CHECK_EQ(problem_text::text(rejected),
                 QStringLiteral("Recovery s1 rejected: Recovery commit failed; pending input is retained for retry."));
        const auto task = Problem(ProblemCode::TaskUnverified).wrapping(ProblemCode::TaskNotAdmitted);
        CHECK_EQ(problem_text::text(task),
                 QStringLiteral("The change did not reach admission.; the outcome of the "
                                "change could not be verified"));
    });
    suite.run("arguments that look like placeholders stay as they are", [] {
        const auto problem = Problem(ProblemCode::GitCustomFilter, {QStringLiteral("%2"), QStringLiteral("a%1b")});
        CHECK_EQ(problem_text::text(problem),
                 QStringLiteral("Outside app: custom filter '%2' is unsupported for a%1b"));
    });
    return suite.report();
}
