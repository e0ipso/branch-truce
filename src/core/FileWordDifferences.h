// File word differences place word-level changes inside the changed passages of one file revision.
#pragma once

#include "ChoiceCatalogue.h"
#include "Correspondence.h"
#include "Draft.h"
#include "FileSnapshot.h"
#include "WordDifferences.h"
#include <map>
#include <vector>

namespace branch_truce {

class FileSession;

// Word ranges for one revision of a file, as byte ranges into each source's
// worktree text and into the correspondence result text.
//
// A changed passage pairs a source range with its result range. While either
// side of a choice section is undecided, its two competing source sections
// pair with each other instead, and so do their mapped result ranges when
// both are mapped. Ranges a competing pair covers come from that pair only.
// Earlier-version passages decorate the earlier pane, never the result.
struct FileWordDifferences {
    std::map<SourceSide, std::vector<ByteRange>> sources;
    std::vector<ByteRange> result;
    WordDifferenceState state = WordDifferenceState::Computed;
    bool operator==(const FileWordDifferences &) const = default;
};

// What the word differences of a revision read.
struct WordDifferenceInputs {
    const FileSnapshots &snapshots;
    const CorrespondenceMap &mapping;
    const ChoiceCatalogue &choices;
    const std::map<std::string, Decision> &decisions;
};

[[nodiscard]] FileWordDifferences fileWordDifferences(const WordDifferenceInputs &inputs);
// The current revision's word differences, or none for a file the app cannot edit.
[[nodiscard]] FileWordDifferences fileWordDifferences(const FileSession &file);

}
