// Owned, current-revision-only input for visual computations; no history or publication state.
#pragma once
#include "../SessionState.h"
#include "EditingPlans.h"

namespace branch_truce::detail {
inline std::size_t captureBytes(const FileSnapshots &snapshots) noexcept
{
    std::size_t bytes = sizeof(snapshots);
    const auto strings = [&](std::initializer_list<const std::string *> values) {
        for (const auto *value : values)
            if (value) bytes += 2 * value->capacity() + 64;
    };
    const auto &p = snapshots.provenance;
    strings({&snapshots.path, &snapshots.resultMode, &p.sessionId, &p.worktreeRoot, &p.gitDirectory, &p.stage2CommitId,
             &p.stage3CommitId, p.baseCommitId ? &*p.baseCommitId : nullptr, &p.headName, &p.originalHead, &p.onto,
             &snapshots.launchResult.text.utf8, &snapshots.launchResult.text.bytes});
    for (const auto *values : {&snapshots.conversion.attributes, &snapshots.conversion.configuration})
        for (const auto &[key, value] : *values) strings({&key, &value});
    for (const auto side : {SourceSide::Earlier, SourceSide::Stage2, SourceSide::Stage3})
        if (const auto &s = snapshots.source(side))
            strings({&s->objectId, &s->mode, &s->rawBytes, &s->worktree.utf8, &s->worktree.bytes,
                     s->commitId ? &*s->commitId : nullptr, &s->label});
    return bytes;
}

struct ComputationInput {
    std::string id, path;
    Revision revision;
    std::optional<FileSnapshots> snapshots;
    ChoiceCatalogue choices;
    std::vector<NonConflictingTarget> targets;
    LaunchFrames frames;
    std::size_t markerSize;
    bool editable;

    explicit ComputationInput(const FileSession &file)
        : id(file.id()),
          path(file.path()),
          revision(file.current()),
          snapshots(file.snapshots() ? std::optional{*file.snapshots()} : std::nullopt),
          choices(file.catalogue()),
          targets(file.nonConflictingTargets()),
          frames(file.planner().frames()),
          markerSize(file.planner().markerSize()),
          editable(file.support() == Support::Supported && !file.pendingAlignment())
    {}
    PlanningInput planning() const
    {
        return {id,
                path,
                revision.id,
                revision.draft,
                snapshots ? &*snapshots : nullptr,
                revision.correspondence.get(),
                choices,
                editable};
    }
    MarkerPlanner planner() const { return {choices, frames, markerSize}; }
    // Charge the current revision, copied captures and maximum admitted derived
    // metadata, with room for planner output and copied ranges. History is never retained.
    static std::size_t retainedBytes(const FileSession &file) noexcept
    {
        return 4
            * (RevisionHistory::retainedBytes(file.current()) + (file.snapshots() ? captureBytes(*file.snapshots()) : 0)
               + 4 * kMaxMetadataBytes);
    }
};
}
