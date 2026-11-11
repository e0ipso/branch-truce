// Session convenience boundary: extract immutable planning inputs and revalidate before application.
#include "EditingCommands.h"
#include "SessionState.h"
#include "internal/EditingPlans.h"

namespace branch_truce {
namespace {
detail::PlanningInput input(const FileSession &file)
{
    return {file.id(),         file.path(),
            file.current().id, file.current().draft,
            file.snapshots(),  file.correspondence(),
            file.catalogue(),  file.support() == Support::Supported && !file.pendingAlignment()};
}
}

ChoicePreview previewChoice(const FileSession &file, RevisionId expected, const std::string &target,
                            ChoiceAction action)
{
    return withinMemory([&]() -> ChoicePreview {
        return detail::planChoice(input(file), file.planner(), file.markers(), expected, target, action);
    });
}

NonConflictingPreview previewNonConflicting(const FileSession &file, RevisionId expected, NonConflictingScope scope)
{
    return withinMemory([&]() -> NonConflictingPreview {
        return detail::planNonConflicting(input(file), file.nonConflictingTargets(), expected, scope);
    });
}

WholeFilePreview previewWholeFile(const FileSession &file, RevisionId expected, SourceSide side)
{
    return withinMemory([&]() -> WholeFilePreview { return detail::planWholeFile(input(file), expected, side); });
}

namespace {
template <typename Preview, typename Planner>
Transition applyPreview(FileSession &file, const Preview &preview, bool confirm, Planner planner)
{
    return withinMemory([&] {
        if (preview.revision != file.current().id) return Transition::StaleRevision;
        const auto current = planner(input(file));
        if (static_cast<const Preview &>(current) != preview) return Transition::PreviewChanged;
        if (current.state != Transition::Applied && current.state != Transition::ConfirmationRequired)
            return current.state;
        if (current.state == Transition::ConfirmationRequired && !confirm) return Transition::ConfirmationRequired;
        return file.applyCommand(current.command(), confirm);
    });
}
}

Transition applyChoice(FileSession &file, const ChoicePreview &preview, bool confirmReplacement)
{
    return applyPreview(file, preview, confirmReplacement, [&](const auto &input) {
        return detail::planChoice(input, file.planner(), file.markers(), preview.revision, preview.target,
                                  preview.action);
    });
}

Transition applyNonConflicting(FileSession &file, const NonConflictingPreview &preview, bool confirm)
{
    return applyPreview(file, preview, confirm, [&](const auto &input) {
        return detail::planNonConflicting(input, file.nonConflictingTargets(), preview.revision, preview.scope);
    });
}

Transition applyWholeFile(FileSession &file, const WholeFilePreview &preview, bool confirm)
{
    return applyPreview(file, preview, confirm, [&](const auto &input) {
        return detail::planWholeFile(input, preview.revision, preview.side);
    });
}
}
