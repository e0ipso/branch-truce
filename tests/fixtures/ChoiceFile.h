#pragma once

#include "core/EditingCommands.h"
#include <stdexcept>

namespace test_support {
inline branch_truce::TextSnapshot choiceText(const std::string &value)
{
    using namespace branch_truce;
    return {value, value,
        {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::Lf, value.ends_with('\n')}};
}

inline branch_truce::FileSession choiceFile(
    std::string left = "header\nleft first\nleft second\nbridge\nleft later\ntail\n",
    std::string right = "header\nright first\nright second\nbridge\nright later\ntail\n",
    std::optional<std::string> result = {})
{
    using namespace branch_truce;
    FileSnapshots captured;
    captured.path = "choices.txt";
    captured.resultMode = "100644";
    captured.provenance.sessionId = "23bd2be7-f830-477a-8077-1e15c4725d09";
    captured.provenance.worktreeRoot = "/disposable-worktree";
    captured.provenance.gitDirectory = "/disposable-worktree/.git";
    captured.provenance.stage2CommitId = "left-commit";
    captured.provenance.stage3CommitId = "right-commit";
    captured.stage2 = SourceSnapshot{"left-object", "100644", left,
        choiceText(left), "left-commit", "Current checkout"};
    captured.stage3 = SourceSnapshot{"right-object", "100644", right,
        choiceText(right), "right-commit", "Merged commit"};
    const auto text = result.value_or(left);
    captured.launchResult.text = choiceText(text);
    const auto path = captured.path;
    return FileSession("choices", path, {text, {}, 0},
        Support::Supported, {}, std::move(captured), true);
}

inline const branch_truce::ChoiceTarget &choiceTarget(const branch_truce::FileSession &file,
    branch_truce::SourceSide side, branch_truce::ChoiceScope scope, std::size_t occurrence = 0)
{
    for (const auto &target : file.choices()) {
        if (target.source.side == side && target.scope == scope) {
            if (occurrence == 0) return target;
            --occurrence;
        }
    }
    throw std::runtime_error("fixture has no requested choice scope");
}

// The user set the result to the common earlier version before launch, so
// choices change text that matches neither side. By default one conflict
// changes three lines on each side.
inline branch_truce::FileSession commonVersionFile(
    std::string left = "header\nleft one\nleft two\nleft three\ntail\n",
    std::string right = "header\nright one\nright two\nright three\ntail\n",
    const std::string &earlier = "header\nbase one\nbase two\nbase three\ntail\n")
{
    using namespace branch_truce;
    auto sources = *choiceFile(std::move(left), std::move(right)).snapshots();
    sources.earlier = SourceSnapshot{"earlier-object", "100644", earlier,
        choiceText(earlier), "earlier-commit", "Common earlier version"};
    sources.launchResult.text = choiceText(earlier);
    return FileSession("choices", sources.path, {earlier, {}, 0},
        Support::Supported, {}, sources, true);
}

inline branch_truce::FileSession batchFile()
{
    return commonVersionFile(
        "header\nleft only\nbridge one\nbase right\nbridge two\nleft conflict\ntail\n",
        "header\nbase left\nbridge one\nright only\nbridge two\nright conflict\ntail\n",
        "header\nbase left\nbridge one\nbase right\nbridge two\nbase conflict\ntail\n");
}

inline branch_truce::FileSession combinationFile(branch_truce::ChoiceScope scope)
{
    return scope == branch_truce::ChoiceScope::Section ? choiceFile() : choiceFile(
        "header\nleft first\nbridge\nleft later\ntail\n",
        "header\nright first\nbridge\nright later\ntail\n");
}
}
