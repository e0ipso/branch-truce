#pragma once

#include "core/EditingCommands.h"
#include <stdexcept>

namespace test_support {
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
    const auto snapshot = [](const std::string &value) {
        return TextSnapshot{value, value,
            {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::Lf, value.ends_with('\n')}};
    };
    captured.stage2 = SourceSnapshot{"left-object", "100644", left,
        snapshot(left), "left-commit", "Current checkout"};
    captured.stage3 = SourceSnapshot{"right-object", "100644", right,
        snapshot(right), "right-commit", "Merged commit"};
    const auto text = result.value_or(left);
    captured.launchResult.text = snapshot(text);
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
}
