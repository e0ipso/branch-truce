#pragma once

#include "core/EditingCommands.h"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace test_support {
inline branch_truce::TextSnapshot choiceText(const std::string &value)
{
    using namespace branch_truce;
    return {value, value, {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::Lf, value.ends_with('\n')}};
}

inline branch_truce::FileSession
choiceFile(const std::string &left = "header\nleft first\nleft second\nbridge\nleft later\ntail\n",
           const std::string &right = "header\nright first\nright second\nbridge\nright later\ntail\n",
           const std::optional<std::string> &result = {})
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
    captured.stage2 =
        SourceSnapshot{"left-object", "100644", left, choiceText(left), "left-commit", "Current checkout"};
    captured.stage3 =
        SourceSnapshot{"right-object", "100644", right, choiceText(right), "right-commit", "Merged commit"};
    const auto text = result.value_or(left);
    captured.launchResult.text = choiceText(text);
    const auto path = captured.path;
    return FileSession("choices", path, {text, {}, 0}, Support::Supported, {}, std::move(captured), true);
}

inline const branch_truce::ChoiceTarget &choiceTarget(const branch_truce::FileSession &file,
                                                      branch_truce::SourceSide side, branch_truce::ChoiceScope scope,
                                                      std::size_t occurrence = 0)
{
    for (const auto &target : file.choices()) {
        if (target.source.side == side && target.scope == scope) {
            if (occurrence == 0) return target;
            --occurrence;
        }
    }
    throw std::runtime_error("fixture has no requested choice scope");
}

// The result Git writes for choiceFile's default sides: each of its two
// sections between conflict markers, with diff3 base sections on request.
inline std::string markedText(bool base = false)
{
    const std::string first = base ? "||||||| base\nbase first\nbase second\n" : "";
    const std::string later = base ? "||||||| base\nbase later\n" : "";
    return "header\n<<<<<<< ours\nleft first\nleft second\n" + first
        + "=======\nright first\nright second\n>>>>>>> theirs\nbridge\n<<<<<<< ours\nleft later\n" + later
        + "=======\nright later\n>>>>>>> theirs\ntail\n";
}

inline branch_truce::FileSession markedFile(bool base = false)
{
    return choiceFile("header\nleft first\nleft second\nbridge\nleft later\ntail\n",
                      "header\nright first\nright second\nbridge\nright later\ntail\n", markedText(base));
}

// The left side has no line where the right side has one, so Git writes an
// empty left region between the conflict markers.
inline branch_truce::FileSession deletionFile()
{
    return choiceFile("header\nbridge\ntail\n", "header\nright first\nbridge\ntail\n",
                      "header\n<<<<<<< ours\n=======\nright first\n>>>>>>> theirs\nbridge\ntail\n");
}

// Both sides change the lines on either side of a line they share, so Git
// writes one block holding two choice sections with `shared` in both sides.
inline std::string splitText()
{
    return "header\n<<<<<<< ours\nleft one\nshared\nleft two\n=======\nright one\nshared\nright two\n"
           ">>>>>>> theirs\ntail\n";
}

inline branch_truce::FileSession splitFile()
{
    return choiceFile("header\nleft one\nshared\nleft two\ntail\n", "header\nright one\nshared\nright two\ntail\n",
                      splitText());
}

// Both sides keep a Markdown heading underlined with exactly seven '=', and
// the file conflicts two lines below it.
inline branch_truce::FileSession headingFile()
{
    return choiceFile("Changes\n=======\nintro\nleft line\ntail\n", "Changes\n=======\nintro\nright line\ntail\n",
                      "Changes\n=======\nintro\n<<<<<<< ours\nleft line\n=======\nright line\n>>>>>>> theirs\ntail\n");
}

// A result Git wrote with merge.conflictStyle=diff3, with the common earlier
// version captured unless `captured` is false.
inline branch_truce::FileSession diff3File(const std::string &left, const std::string &right,
                                           const std::string &earlier, std::string result, bool captured = true)
{
    using namespace branch_truce;
    auto sources = *choiceFile(left, right, result).snapshots();
    if (captured)
        sources.earlier = SourceSnapshot{"earlier-object",    "100644",         earlier,
                                         choiceText(earlier), "earlier-commit", "Common earlier version"};
    return FileSession("choices", sources.path, {std::move(result), {}, 0}, Support::Supported, {}, sources, true);
}

// A process's standard output and its wait status, which is -1 when the
// process could not be started.
struct ProcessOutput {
    std::string text;
    int status = -1;
};

// Runs `arguments` from the PATH without a shell and reads its standard
// output to the end.
inline ProcessOutput readProcess(std::vector<std::string> arguments)
{
    ProcessOutput output;
    std::array<int, 2> ends{};
    if (pipe(ends.data()) != 0) return output;
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, ends[0]);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    pid_t child = 0;
    const bool started = posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    close(ends[1]);
    std::array<char, 4096> buffer{};
    while (started) {
        const auto count = read(ends[0], buffer.data(), buffer.size());
        if (count == 0 || (count < 0 && errno != EINTR)) break;
        if (count > 0) output.text.append(buffer.data(), static_cast<std::size_t>(count));
    }
    // Closing the read end first lets a child still writing end on SIGPIPE.
    close(ends[0]);
    if (!started) return output;
    int status = 0;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) return output;
    }
    output.status = status;
    return output;
}

// What `git merge-file --diff3` on the PATH writes for three versions, with
// the labels a merge gives them. Its exit status counts the conflicts, so one
// above 127 or a signal is a failure.
inline std::string gitDiff3(const std::string &left, const std::string &earlier, const std::string &right)
{
    auto pattern = (std::filesystem::temp_directory_path() / "branch-truce-diff3.XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("cannot create a directory for git merge-file");
    const std::filesystem::path directory(pattern);
    std::vector<std::string> arguments{"git",  "merge-file", "--diff3", "-p", "-L",
                                       "ours", "-L",         "base",    "-L", "theirs"};
    const std::pair<const char *, const std::string *> versions[] = {
        {"left", &left}, {"base", &earlier}, {"right", &right}};
    arguments.reserve(arguments.size() + std::size(versions));
    for (const auto &[name, text] : versions) {
        std::ofstream(directory / name, std::ios::binary) << *text;
        arguments.push_back((directory / name).string());
    }
    const auto merged = readProcess(std::move(arguments));
    std::filesystem::remove_all(directory);
    if (merged.status == -1 || !WIFEXITED(merged.status) || WEXITSTATUS(merged.status) > 127)
        throw std::runtime_error("git merge-file failed");
    return merged.text;
}

// The common earlier version underlines a heading with exactly seven '='
// inside the hunk both sides change, so Git copies that line into the base
// region of a diff3 block, two lines above its own separator.
inline branch_truce::FileSession underlinedBaseFile(bool captured = true)
{
    return diff3File("intro\nLeft title\n-----------\nleft body\ntail\n",
                     "intro\nRight title\n************\nright body\ntail\n", "intro\nTitle\n=======\nbody\ntail\n",
                     "intro\n<<<<<<< ours\nLeft title\n-----------\nleft body\n||||||| base\nTitle\n=======\nbody\n"
                     "=======\nRight title\n************\nright body\n>>>>>>> theirs\ntail\n",
                     captured);
}

// The common earlier version ends the hunk both sides change with a heading
// underline of exactly seven '=', so Git writes it directly above its own
// separator, and the earlier version's underline could be either line. With
// `deleted`, the right side removes the hunk and its region is empty.
inline branch_truce::FileSession underlineEndedBaseFile(bool captured = true, bool deleted = false)
{
    return diff3File("intro\nLeft\n-----\ntail\n", deleted ? "intro\ntail\n" : "intro\nRight\n*****\ntail\n",
                     "intro\nTitle\n=======\ntail\n",
                     std::string("intro\n<<<<<<< ours\nLeft\n-----\n||||||| base\nTitle\n=======\n=======\n")
                         + (deleted ? "" : "Right\n*****\n") + ">>>>>>> theirs\ntail\n",
                     captured);
}

// The user set the result to the common earlier version before launch, so
// choices change text that matches neither side. By default one conflict
// changes three lines on each side.
inline branch_truce::FileSession
commonVersionFile(const std::string &left = "header\nleft one\nleft two\nleft three\ntail\n",
                  const std::string &right = "header\nright one\nright two\nright three\ntail\n",
                  const std::string &earlier = "header\nbase one\nbase two\nbase three\ntail\n")
{
    using namespace branch_truce;
    auto sources = *choiceFile(left, right).snapshots();
    sources.earlier = SourceSnapshot{"earlier-object",    "100644",         earlier,
                                     choiceText(earlier), "earlier-commit", "Common earlier version"};
    sources.launchResult.text = choiceText(earlier);
    return FileSession("choices", sources.path, {earlier, {}, 0}, Support::Supported, {}, sources, true);
}

inline branch_truce::FileSession batchFile()
{
    return commonVersionFile("header\nleft only\nbridge one\nbase right\nbridge two\nleft conflict\ntail\n",
                             "header\nbase left\nbridge one\nright only\nbridge two\nright conflict\ntail\n",
                             "header\nbase left\nbridge one\nbase right\nbridge two\nbase conflict\ntail\n");
}

inline branch_truce::FileSession combinationFile(branch_truce::ChoiceScope scope)
{
    return scope == branch_truce::ChoiceScope::Section ? choiceFile()
                                                       : choiceFile("header\nleft first\nbridge\nleft later\ntail\n",
                                                                    "header\nright first\nbridge\nright later\ntail\n");
}
}
