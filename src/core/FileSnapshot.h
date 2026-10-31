// File snapshots retain the captured text, formatting and source identities used throughout a session.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace branch_truce {

// Value records for the engine. Platform adapters translate codecs and paths
// without introducing Qt or filesystem handles into session state.
struct SnapshotFormat {
    enum class Encoding { Utf8, Utf16LittleEndian, Utf16BigEndian };
    enum class Newline { Lf, CrLf };
    Encoding encoding = Encoding::Utf8;
    bool bom = false;
    Newline newline = Newline::Lf;
    bool finalNewline = false;
    bool operator==(const SnapshotFormat &) const = default;
};

struct TextSnapshot {
    std::string utf8; // Editable representation: UTF-8, LF, no encoding BOM.
    std::string bytes; // Exact captured worktree representation, before decoding.
    SnapshotFormat format;
    bool operator==(const TextSnapshot &) const = default;
};

// Observed identity, not authority to publish without fresh guarded checks.
struct SnapshotFileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint32_t mode = 0;
    std::int64_t size = 0;
    std::int64_t modifiedSeconds = 0;
    std::int64_t modifiedNanoseconds = 0;
    std::int64_t changedSeconds = 0;
    std::int64_t changedNanoseconds = 0;
    bool operator==(const SnapshotFileIdentity &) const = default;
};

struct SourceSnapshot {
    std::string objectId; // Opaque raw-stage identity, without a fixed hash length.
    std::string mode; // This stage's mode, independently of the result's mode.
    std::string rawBytes;
    TextSnapshot worktree;
    std::optional<std::string> commitId; // Only a verified available origin.
    std::string label;
    bool operator==(const SourceSnapshot &) const = default;
};

struct SnapshotProvenance {
    enum class Operation { Merge, Rebase };
    std::string sessionId;
    std::string worktreeRoot;
    std::string gitDirectory;
    Operation operation = Operation::Merge;
    std::string stage2CommitId;
    std::string stage3CommitId;
    std::optional<std::string> baseCommitId;
    std::string headName;
    std::string originalHead;
    std::string onto;
    bool operator==(const SnapshotProvenance &) const = default;
};

struct ConversionSnapshot {
    std::map<std::string, std::string> attributes;
    std::map<std::string, std::string> configuration;
    bool operator==(const ConversionSnapshot &) const = default;
};

struct LaunchSnapshot {
    TextSnapshot text;
    SnapshotFileIdentity identity;
    bool operator==(const LaunchSnapshot &) const = default;
};

struct FileSnapshots {
    std::string path; // Exact repository-relative filename bytes.
    SnapshotProvenance provenance;
    ConversionSnapshot conversion; // Validated policy shared by every source.
    std::string resultMode; // Agreed stage-2/stage-3 mode.
    // Absence is distinct from a present source with empty raw/worktree bytes.
    std::optional<SourceSnapshot> earlier;
    std::optional<SourceSnapshot> stage2;
    std::optional<SourceSnapshot> stage3;
    LaunchSnapshot launchResult;
    bool operator==(const FileSnapshots &) const = default;
};

}
