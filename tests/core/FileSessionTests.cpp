// FileSession transactions: launch, history, discard and the large-file gate.
#include "TestHarness.h"
#include "core/ResourceLimits.h"
#include "core/SessionState.h"
#include <string>

using namespace branch_truce;

namespace {
std::string lines(std::size_t count)
{
    std::string text;
    text.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) text += "x\n";
    return text;
}

FileSession captured(const std::string &text)
{
    FileSnapshots sources;
    sources.path = "limit.txt";
    sources.provenance.sessionId = "limits";
    sources.stage2 = SourceSnapshot{"source-object", "100644", text, {text, text, {}}, "source-commit", "Source"};
    sources.launchResult.text = {text, text, {}};
    return FileSession("limit", sources.path, {text, {}, 0}, Support::Supported, {}, sources);
}

void discardAfterLoadingALargeFileRestoresItsLaunchText()
{
    const auto text = lines(kLargeFileLines + 1);
    auto file = captured(text);
    CHECK(file.support() == Support::LargeFile);
    CHECK(file.loadLargeFile() == Transition::Applied);
    const auto loaded = file.current().id;
    CHECK(file.editResult(loaded, {{0, 1}, "y"}) == Transition::Applied);
    CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
    CHECK(file.current().draft.text == text && file.current().result == loaded);
    CHECK(file.correspondence() && file.correspondence()->text() == text);
    CHECK(file.undo(file.current().id) == Transition::Applied && file.current().draft.text.starts_with('y'));
}
}

int main()
{
    test_support::Suite suite("FileSessionTests");
    suite.run("discarding after loading a large file restores its launch text",
              discardAfterLoadingALargeFileRestoresItsLaunchText);
    return suite.report();
}
