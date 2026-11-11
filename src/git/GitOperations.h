// Git operations are described once each: their operands, the policy checks before them, and the steps they run.
#pragma once

// Private to the branch_truce_git library.

#include "GitAdapter.h"

#include <QByteArray>
#include <QList>

namespace branch_truce::git::detail {

// What runs after the configuration, worktree and object-store checks every operation shares.
enum class Flow {
    Read, // The operation, then the result.
    Discovery, // RepositoryDiscovery's steps.
    Canonicalize, // The operation in a private object store, then the size, blob and checkout reads.
    Preflight, // The hooks check, then the result; the operation itself never runs.
    Stage // The hooks check, an index baseline, the operation and an index reinspection.
};

// How the operation's own output becomes part of the result.
enum class Output {
    Raw, // Kept as standard output.
    Attributes, // The path's conversion attributes.
    IndexEntries, // NUL-delimited index records.
    CanonicalObject // The object id hash-object wrote.
};

struct OperationSpec {
    bool path = false; // Takes one repository-relative path.
    bool object = false; // Takes one full object id.
    bool input = false; // Takes worktree bytes on standard input, possibly empty.
    bool filters = false; // The path must have no custom filter driver configured.
    bool staging = false; // A filesystem monitor is refused and the hooks are inspected.
    Flow flow = Flow::Read;
    Output output = Output::Raw;
    QList<QByteArray> (*arguments)(const Request &request) = nullptr;
};

// The description of a named operation, or nullptr for a value outside the enum.
[[nodiscard]] const OperationSpec *operationSpec(Operation operation);
[[nodiscard]] QList<QByteArray> indexArguments(const QByteArray &path);

} // namespace branch_truce::git::detail
