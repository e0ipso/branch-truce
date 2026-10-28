// Phase sequence for GitAdapter. afterProcess runs the checks every phase
// shares, then the current phase's handler validates its output and either
// launches the next phase or completes the request.
#include "GitAdapterState.h"
#include "GitParsing.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <optional>
#include <unistd.h>

namespace branch_truce::git {
namespace {
using detail::inspectionLimit;
using detail::objectId;
using detail::outputLimit;
using detail::parseIndex;

bool regularFile(const QString &directory, const QByteArray &path)
{
    // QFile's QString boundary cannot address arbitrary Linux filename bytes.
    // Inspect each raw component so a display conversion cannot select a decoy.
    QByteArray current = QFile::encodeName(directory);
    const auto parts = path.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        current += '/' + parts[i];
        struct stat info {};
        if (::lstat(current.constData(), &info) != 0
            || (i + 1 == parts.size() ? !S_ISREG(info.st_mode) : !S_ISDIR(info.st_mode))) {
            return false;
        }
    }
    return true;
}
std::optional<QString> responsePath(const QByteArray &output)
{
    if (!output.endsWith('\n')) return std::nullopt;
    const auto bytes = output.first(output.size() - 1);
    const auto path = QFile::decodeName(bytes);
    if (bytes.isEmpty() || bytes.contains('\0') || QFile::encodeName(path) != bytes
        || !QDir::isAbsolutePath(path)) return std::nullopt;
    return path;
}
bool localObjectStores(const QString &objects, QString &diagnostic)
{
    // Git 2.43 has no reliable no-lazy-fetch switch. Inspect the common object
    // store and its alternates without asking Git to open any object first.
    QStringList pending{objects};
    QSet<QString> seen;
    qsizetype alternateBytes = 0;
    while (!pending.isEmpty()) {
        const auto path = QFileInfo(pending.takeLast()).canonicalFilePath();
        if (path.isEmpty() || !QFileInfo(path).isDir()
            || ::access(QFile::encodeName(path).constData(), R_OK | X_OK) != 0) {
            diagnostic = QStringLiteral("Outside app: object storage cannot be inspected offline");
            return false;
        }
        if (seen.contains(path)) continue;
        seen.insert(path);
        if (seen.size() > 256) {
            diagnostic = QStringLiteral("Outside app: too many alternate object stores to inspect");
            return false;
        }
        const auto packPath = path + QStringLiteral("/pack");
        const QFileInfo packInfo(packPath);
        if (packInfo.exists() || packInfo.isSymLink()) {
            if (!packInfo.isDir() || ::access(QFile::encodeName(packPath).constData(), R_OK | X_OK) != 0) {
                diagnostic = QStringLiteral("Outside app: object packs cannot be inspected offline");
                return false;
            }
            if (!QDir(packPath).entryList({QStringLiteral("*.promisor")},
                                         QDir::Files | QDir::Hidden | QDir::System).isEmpty()) {
                diagnostic = QStringLiteral("Outside app: promisor object packs require unsupported lazy fetching");
                return false;
            }
        }
        QFile alternates(path + QStringLiteral("/info/alternates"));
        const QFileInfo alternateInfo(alternates.fileName());
        if (!alternateInfo.exists() && !alternateInfo.isSymLink()) continue;
        if (!alternates.open(QIODevice::ReadOnly)) {
            diagnostic = QStringLiteral("Outside app: alternate object stores cannot be inspected");
            return false;
        }
        const auto bytes = alternates.read(inspectionLimit + 1);
        alternateBytes += bytes.size();
        if (alternates.error() != QFileDevice::NoError || alternateBytes > inspectionLimit) {
            diagnostic = QStringLiteral("Outside app: alternate object store list is unreadable or oversized");
            return false;
        }
        for (const auto &line : bytes.split('\n')) {
            if (line.isEmpty() || line.startsWith('#')) continue;
            const auto decoded = QFile::decodeName(line);
            // Fail closed on Git's quoted alternate syntax rather than inspect
            // a different directory. Ordinary relative/absolute alternates work.
            if (line.startsWith('"') || line.contains('\0') || QFile::encodeName(decoded) != line) {
                diagnostic = QStringLiteral("Outside app: alternate object path cannot be inspected losslessly");
                return false;
            }
            pending.append(QDir::isAbsolutePath(decoded) ? decoded : path + QLatin1Char('/') + decoded);
        }
    }
    return true;
}
} // namespace

void GitAdapter::State::afterProcess(bool normal, int code)
{
    if (!active) {
        return;
    }
    closeLaunchPipe();
    if (nativeLaunchFailed) normal = false;
    if (phase == Phase::Inspection) {
        handleInspection(normal, code);
        return;
    }
    // Keep failed preflight output as well as the requested operation's
    // output. Reinspection has its own fields and must not overwrite it.
    result.standardOutput = phaseOutput;
    if (phase == Phase::Operation) {
        result.exitCode = normal ? code : -1;
    }
    if (phase == Phase::Discovery && normal && !stopping) {
        handleDiscovery(code);
        return;
    }
    if (!stopping && (!normal || code != 0)) {
        if (result.outcome != Outcome::LaunchFailure) {
            result.outcome = normal ? Outcome::ExitFailure : Outcome::ProcessFailure;
            result.diagnostic = normal ? QStringLiteral("Git exited with code %1").arg(code)
                                       : QStringLiteral("Git process crashed");
        }
        stopping = true;
    }
    if (stopping) {
        if (mutationStarted) {
            inspectMutation();
        } else {
            complete();
        }
        return;
    }
    switch (phase) {
    case Phase::Version: handleVersion(); break;
    case Phase::Configuration: handleConfiguration(); break;
    case Phase::PolicyRoot: handlePolicyRoot(); break;
    case Phase::ObjectDirectory: handleObjectDirectory(); break;
    case Phase::TrackedPaths: handleTrackedPaths(); break;
    case Phase::Attributes: handleAttributes(); break;
    case Phase::GitDirectory: handleGitDirectory(); break;
    case Phase::Hooks: handleHooks(); break;
    case Phase::Baseline: handleBaseline(); break;
    case Phase::Operation: handleOperation(); break;
    case Phase::CanonicalSize: handleCanonicalSize(); break;
    case Phase::CanonicalObject: handleCanonicalObject(); break;
    case Phase::CanonicalCheckout: handleCanonicalCheckout(); break;
    case Phase::Inspection:
    case Phase::Discovery:
        // Not reached: both phases return before this switch.
        break;
    }
}
void GitAdapter::State::afterPolicy()
{
    if (result.request.operation == Operation::DiscoverRepository) {
        discovery = std::make_unique<detail::RepositoryDiscovery>(result, directory);
        launch(Phase::Discovery, discovery->arguments());
    } else if (isMutation()) {
        launch(Phase::GitDirectory, {"rev-parse", "--absolute-git-dir"});
    } else if (stagingPolicy()) {
        launch(Phase::Hooks, {"rev-parse", "--path-format=absolute", "--git-path", "hooks"});
    } else if (result.request.operation == Operation::CanonicalizeText) {
        temporaryObjects = std::make_unique<QTemporaryDir>(QStringLiteral("/tmp/branch-truce-conversion-XXXXXX"));
        if (!temporaryObjects->isValid()) {
            reject(QStringLiteral("Could not create private conversion object storage"));
            return;
        }
        launch(Phase::Operation, operationArguments(), false, result.request.input);
    } else {
        launch(Phase::Operation, operationArguments());
    }
}
void GitAdapter::State::handleVersion()
{
    if (!phaseOutput.startsWith("git version ")) {
        reject(QStringLiteral("Selected executable did not identify itself as Git"));
    } else {
        launch(Phase::Configuration, {"config", "--null", "--list", "--includes"}, true);
    }
}
void GitAdapter::State::handleConfiguration()
{
    if (!checkConfiguration()) {
        reject(result.diagnostic);
    } else if (result.request.operation == Operation::ReadConfiguration) {
        result.standardOutput = phaseOutput;
        result.exitCode = 0;
        result.outcome = Outcome::Success;
        complete();
    } else if (objectPolicy()) {
        launch(Phase::PolicyRoot, {"rev-parse", "--path-format=absolute", "--show-toplevel"});
    } else {
        afterPolicy();
    }
}
void GitAdapter::State::handlePolicyRoot()
{
    const auto root = responsePath(phaseOutput);
    if (!root || !QFileInfo(*root).isDir()) {
        reject(QStringLiteral("Outside app: invalid worktree root response"));
        return;
    }
    const auto prefix = *root == QLatin1String("/") ? *root : *root + QLatin1Char('/');
    if (executable.startsWith(prefix)) {
        reject(QStringLiteral("The trusted Git executable must be outside the repository"));
        return;
    }
    // Request paths are repository-relative. Status also examines the
    // whole worktree even when Git is launched from a subdirectory.
    directory = *root;
    result.worktreeRoot = *root;
    if (isMutation() && !regularFile(directory, result.request.path)) {
        reject(QStringLiteral("Staging requires one regular file without symlink traversal"));
    } else {
        launch(Phase::ObjectDirectory, {"rev-parse", "--path-format=absolute", "--git-path", "objects"});
    }
}
void GitAdapter::State::handleObjectDirectory()
{
    const auto objects = responsePath(phaseOutput);
    if (!objects) {
        reject(QStringLiteral("Outside app: invalid object directory response"));
    } else if (!localObjectStores(*objects, result.diagnostic)) {
        reject(result.diagnostic);
    } else {
        originalObjectDirectory = *objects;
        if (result.request.operation == Operation::Status) {
            if (hasFilterConfiguration()) launch(Phase::TrackedPaths, {"ls-files", "--cached", "-z"});
            else afterPolicy();
        } else if (filterPolicy()) {
            launch(Phase::Attributes, {"check-attr", "-z", "--all", "--", result.request.path});
        } else {
            afterPolicy();
        }
    }
}
void GitAdapter::State::handleTrackedPaths()
{
    if (!phaseOutput.isEmpty() && !phaseOutput.endsWith('\0')) {
        reject(QStringLiteral("Outside app: invalid tracked path inspection"));
    } else if (phaseOutput.isEmpty()) {
        afterPolicy();
    } else {
        const auto paths = phaseOutput;
        // --all distinguishes an absent filter from filter=unspecified.
        launch(Phase::Attributes, {"check-attr", "-z", "--all", "--stdin"}, false, paths);
    }
}
void GitAdapter::State::handleAttributes()
{
    if (!checkFilters()) reject(result.diagnostic);
    else afterPolicy();
}
void GitAdapter::State::handleGitDirectory()
{
    // Remove Git's delimiter, preserving newlines in the directory name.
    if (!phaseOutput.endsWith('\n')) {
        reject(QStringLiteral("Invalid Git directory response"));
        return;
    }
    phaseOutput.chop(1);
    const auto decoded = QFile::decodeName(phaseOutput);
    if (QFile::encodeName(decoded) != phaseOutput) {
        reject(QStringLiteral("Git directory cannot round-trip through Qt's directory API"));
        return;
    }
    const auto key = QFileInfo(decoded).canonicalFilePath();
    if (key.isEmpty()) {
        reject(QStringLiteral("Git directory is unavailable"));
        return;
    }
    reserveMutation(key);
    if (mutationKey.isEmpty()) {
        reject(result.diagnostic);
    } else {
        launch(Phase::Hooks, {"rev-parse", "--path-format=absolute", "--git-path", "hooks"});
    }
}
void GitAdapter::State::handleHooks()
{
    if (!phaseOutput.endsWith('\n')) {
        reject(QStringLiteral("Outside app: invalid hooks directory response"));
        return;
    }
    phaseOutput.chop(1);
    const auto decoded = QFile::decodeName(phaseOutput);
    if (QFile::encodeName(decoded) != phaseOutput) {
        reject(QStringLiteral("Outside app: hooks directory cannot round-trip through Qt's directory API"));
        return;
    }
    const QFileInfo info(decoded);
    const QDir hooks(info.isAbsolute() ? info.filePath() : directory + QLatin1Char('/') + info.filePath());
    if (hooks.exists()) {
        if (!QFileInfo(hooks.path()).isReadable()) {
            reject(QStringLiteral("Outside app: hooks directory cannot be inspected"));
            return;
        }
        // git add and its mergetool parent run post-index-change, not
        // commit/checkout hooks. Inspect the effective core.hooksPath.
        const QFileInfo hook(hooks.filePath(QStringLiteral("post-index-change")));
        if (hook.isExecutable()) {
            reject(QStringLiteral("Outside app: executable post-index-change hook is unsupported"));
            return;
        }
    } else if ((info.exists() || info.isSymLink()) && info.canonicalFilePath() != QLatin1String("/dev/null")) {
        reject(QStringLiteral("Outside app: hooks path is not an inspectable directory"));
        return;
    }
    if (isMutation()) {
        launch(Phase::Baseline, indexArguments());
    } else {
        result.standardOutput.clear();
        result.outcome = Outcome::Success;
        result.exitCode = 0;
        complete();
    }
}
void GitAdapter::State::handleBaseline()
{
    if (!validIndex(phaseOutput, false) || phaseOutput.size() > inspectionLimit) {
        reject(QStringLiteral("Unsupported or oversized index baseline"));
    } else {
        result.indexBefore = phaseOutput;
        launch(Phase::Operation, operationArguments());
    }
}
void GitAdapter::State::handleOperation()
{
    result.outcome = Outcome::Success;
    if (result.request.operation == Operation::ReadAttributes && !captureAttributes()) {
        reject(QStringLiteral("Outside app: invalid conversion attribute inspection"));
        return;
    }
    if (result.request.operation == Operation::CanonicalizeText) {
        if (!phaseOutput.endsWith('\n') || !objectId(phaseOutput.first(phaseOutput.size() - 1))) {
            reject(QStringLiteral("Invalid canonical object response"));
        } else {
            result.canonicalObjectId = phaseOutput.first(phaseOutput.size() - 1);
            launch(Phase::CanonicalSize, {"cat-file", "-s", result.canonicalObjectId});
        }
        return;
    }
    if (result.request.operation == Operation::ListUnmerged || result.request.operation == Operation::InspectIndex) {
        const auto entries = parseIndex(phaseOutput);
        if (!entries) {
            result.outcome = Outcome::InspectionFailure;
            result.diagnostic = QStringLiteral("Invalid NUL-delimited index records");
        } else {
            result.indexEntries = *entries;
        }
    }
    if (mutationStarted) {
        inspectMutation();
    } else {
        complete();
    }
}
void GitAdapter::State::handleDiscovery(int code)
{
    result.exitCode = code;
    if (discovery->consume(phaseOutput, code)) {
        launch(Phase::Discovery, discovery->arguments());
    } else {
        complete();
    }
}
void GitAdapter::State::handleInspection(bool normal, int code)
{
    result.indexAfter = phaseOutput;
    if (!stopping && normal && code == 0 && validIndex(phaseOutput, false)) {
        result.mutation = result.indexBefore == result.indexAfter
            ? Mutation::ObservedUnchanged : Mutation::ObservedChanged;
        if (result.outcome == Outcome::Success && !validIndex(phaseOutput, true)) {
            result.outcome = Outcome::InspectionFailure;
            result.diagnostic = QStringLiteral("Staging exited successfully without a single regular stage-zero entry");
        }
    } else {
        inspectionFailed(QStringLiteral("Index reinspection failed; mutation remains uncertain"));
    }
    complete();
}
void GitAdapter::State::handleCanonicalSize()
{
    bool valid = false;
    const auto size = phaseOutput.toULongLong(&valid);
    if (!valid || QByteArray::number(size) + '\n' != phaseOutput) {
        reject(QStringLiteral("Invalid canonical object size response"));
    } else if (size > static_cast<qulonglong>(outputLimit)) {
        reject(QStringLiteral("Canonical text size %1 bytes after clean conversion exceeds the %2 byte output limit").arg(size).arg(outputLimit));
    } else {
        launch(Phase::CanonicalObject, {"cat-file", "blob", result.canonicalObjectId});
    }
}
void GitAdapter::State::handleCanonicalObject()
{
    result.canonicalBytes = phaseOutput;
    launch(Phase::CanonicalCheckout, {"cat-file", "--filters", "--path=" + result.request.path, result.canonicalObjectId});
}
void GitAdapter::State::handleCanonicalCheckout()
{
    result.outcome = Outcome::Success;
    complete();
}
} // namespace branch_truce::git
