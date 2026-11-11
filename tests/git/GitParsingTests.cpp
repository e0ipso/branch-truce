// The parsers every Git reply goes through: object ids, repository-relative filenames and
// `ls-files --stage -z` records. One row per input, so a new edge case is one more row.
#include "TestHarness.h"
#include "git/GitParsing.h"

#include <QByteArray>

#include <string>

using namespace branch_truce::git;

namespace {
const QByteArray &sha1()
{
    static const QByteArray value(40, 'a');
    return value;
}
const QByteArray &sha256()
{
    static const QByteArray value(64, 'b');
    return value;
}
constexpr char nul = '\0';

QByteArray record(const QByteArray &mode, const QByteArray &id, const QByteArray &stage, const QByteArray &path)
{
    return mode + ' ' + id + ' ' + stage + '\t' + path + nul;
}

void objectIdsAreFullSha1OrSha256Hex()
{
    for (const auto &id :
         {sha1(), sha256(), QByteArray(40, 'F'), QByteArray("0123456789abcdefABCDEF0123456789abcdef01")}) {
        SCOPE(id.toStdString());
        CHECK(detail::objectId(id));
    }
    for (const auto &id : {QByteArray(), QByteArray(39, 'a'), QByteArray(41, 'a'), QByteArray(63, 'a'),
                           QByteArray(65, 'a'), QByteArray(39, 'a') + 'g', QByteArray("HEAD"), QByteArray("--help")}) {
        SCOPE(id.toStdString());
        CHECK(!detail::objectId(id));
    }
}

void relativeFilesAreExactNamesInsideTheWorktree()
{
    for (const auto &path :
         {QByteArray("a"), QByteArray("dir/file"), QByteArray(":leading"), QByteArray("-"), QByteArray("dir/.hidden"),
          QByteArray("..dots"), QByteArray("dots.."), QByteArray("odd-") + char(0xff), QByteArray("tab\there")}) {
        SCOPE(path.toStdString());
        CHECK(detail::relativeFile(path));
    }
    for (const auto &path :
         {QByteArray(), QByteArray("/absolute"), QByteArray("dir/"), QByteArray("a//b"), QByteArray("./a"),
          QByteArray("a/./b"), QByteArray("../a"), QByteArray("a/.."), QByteArray("a\0b", 3)}) {
        SCOPE(path.toStdString());
        CHECK(!detail::relativeFile(path));
    }
}

void wellFormedIndexRecordsKeepEveryField()
{
    CHECK(detail::parseIndex({}).has_value());
    const auto empty = detail::parseIndex({});
    if (!empty) throw test_support::Failure("An empty Git index reply did not parse");
    CHECK(empty->isEmpty());
    const auto parsed = detail::parseIndex(record("100644", sha1(), "1", "a") + record("100755", sha256(), "3", "x\ty")
                                           + record("120000", sha1(), "0", QByteArray("odd-") + char(0xfe)));
    CHECK(parsed.has_value());
    if (!parsed) throw test_support::Failure("The well-formed Git index reply did not parse");
    CHECK_EQ(parsed->size(), 3);
    const auto &first = parsed->at(0);
    CHECK_EQ(first.mode, QByteArray("100644"));
    CHECK_EQ(first.objectId, sha1());
    CHECK_EQ(first.stage, 1u);
    CHECK_EQ(first.path, QByteArray("a"));
    // Only the first tab separates the fields; later tabs belong to the filename.
    CHECK_EQ(parsed->at(1).path, QByteArray("x\ty"));
    CHECK_EQ(parsed->at(1).objectId, sha256());
    CHECK_EQ(parsed->at(1).stage, 3u);
    CHECK_EQ(parsed->at(2).path, QByteArray("odd-") + char(0xfe));
}

void malformedIndexRecordsRejectTheWholeReply()
{
    const std::pair<const char *, QByteArray> malformed[] = {
        {"no terminating NUL", record("100644", sha1(), "0", "a").chopped(1)},
        {"no tab", QByteArray("100644 ") + sha1() + " 0 a" + nul},
        {"a five-digit mode", record("10064", sha1(), "0", "a")},
        {"a non-octal mode", record("100648", sha1(), "0", "a")},
        {"stage 4", record("100644", sha1(), "4", "a")},
        {"a two-digit stage", record("100644", sha1(), "12", "a")},
        {"a short object id", record("100644", QByteArray(39, 'a'), "0", "a")},
        {"two fields", QByteArray("100644 ") + sha1() + "\ta" + nul},
        {"an extra space", QByteArray("100644  ") + sha1() + " 0\ta" + nul},
        {"an empty path", record("100644", sha1(), "0", {})},
        {"a path with ..", record("100644", sha1(), "0", "a/../b")},
        {"an absolute path", record("100644", sha1(), "0", "/etc/passwd")},
        {"a good record before a bad one", record("100644", sha1(), "0", "a") + record("100644", sha1(), "9", "b")}};
    for (const auto &[name, bytes] : malformed) {
        SCOPE(name);
        CHECK(!detail::parseIndex(bytes).has_value());
    }
}
} // namespace

int main(int argc, char **argv)
{
    test_support::configure(argc, argv);
    test_support::Suite suite("GitParsingTests");
    suite.run("object ids are full SHA-1 or SHA-256 hex", objectIdsAreFullSha1OrSha256Hex);
    suite.run("relative files are exact names inside the worktree", relativeFilesAreExactNamesInsideTheWorktree);
    suite.run("well-formed index records keep every field", wellFormedIndexRecordsKeepEveryField);
    suite.run("malformed index records reject the whole reply", malformedIndexRecordsRejectTheWholeReply);
    return suite.report();
}
