// Resource limits: the text envelope at the line, payload and large-file boundaries.
#include "TestHarness.h"
#include "core/ResourceLimits.h"
#include "core/SessionFixtures.h"
#include <string>

using namespace branch_truce;
using test_support::lines;
using test_support::longestLines;

namespace {
// Empty results still retain decisions. Large keys test metadata accounting
// independently from the entry cap, and publication holds survive both limits.
void emptyHistoryIsBoundedAndKeepsProtectedRevisions(std::size_t keyBytes, std::size_t count)
{
    const std::string key(keyBytes, 'd');
    FileSession file("id", "file", {"", {{key, {}}}, 0});
    for (std::size_t i = 0; i < count; ++i) {
        const Decision decision{i % 2 ? DecisionValue::Ignored : DecisionValue::Accepted, {}};
        CHECK(file.replaceDraft(file.current().id, {"", {{key, decision}}, 0}) == Transition::Applied);
        if (i == 0) {
            CHECK(file.beginMarkResolved(2) == Transition::Applied);
            CHECK(file.recordCheckpoint(2, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordWrite(2, Outcome::Succeeded) == Transition::Applied);
            CHECK(file.recordStage(2, Outcome::Succeeded, "object", "100644") == Transition::Applied);
        }
        if (i == 1) CHECK(file.recordCheckpoint(3, Outcome::Succeeded) == Transition::Applied);
    }
    CHECK(file.revision(1) && file.revision(2) && file.revision(3) && !file.revision(4));
    std::size_t undone = 0;
    while (file.canUndo()) {
        const auto before = file.current().draft;
        CHECK(file.undo(file.current().id) == Transition::Applied);
        CHECK(file.current().draft != before);
        ++undone;
    }
    CHECK(undone > 0 && undone < count);
    CHECK(file.redo(file.current().id) == Transition::Applied);
    CHECK(file.discardToCheckpoint(file.current().id) == Transition::Applied);
    CHECK(file.current().draft.decisions.at(key).value == DecisionValue::Ignored);
    CHECK(file.discardToStaged(file.current().id) == Transition::Applied);
    CHECK(file.current().draft.decisions.at(key).value == DecisionValue::Accepted);
}

void theLargeFileThresholdIsNotATextLimit()
{
    CHECK(assessText(lines(kLargeFileLines - 1)).limit == TextLimit::Within);
    CHECK(assessText(lines(kLargeFileLines)).limit == TextLimit::Within);
    CHECK(assessText(lines(kLargeFileLines + 1)).limit == TextLimit::Within);
}

void lineCountCountsAFinalLineWithoutANewline()
{
    CHECK(lineCount(lines(kLargeFileLines + 1)) == kLargeFileLines + 1 && lineCount("a\nb") == 2 && lineCount("") == 0);
}

void theLineLimitRejectsTheNextByte()
{
    const auto belowLine = std::string(kMaxLineBytes - 1, 'a');
    const auto atLine = std::string(kMaxLineBytes, 'a');
    const auto aboveLine = std::string(kMaxLineBytes + 1, 'a');
    CHECK(assessText(belowLine).limit == TextLimit::Within);
    CHECK(assessText(atLine).limit == TextLimit::Within);
    CHECK(assessText(aboveLine).limit == TextLimit::LineBytes);
    CHECK(withinTextEnvelope(atLine) && !withinTextEnvelope(aboveLine));
}

void thePayloadLimitRejectsTheNextLine()
{
    const auto fits = kMaxRecoveryPayloadBytes / kMaxLineBytes;
    const auto atPayload = longestLines(fits);
    const auto abovePayload = longestLines(fits + 1);
    CHECK(atPayload.size() == kMaxRecoveryPayloadBytes
          && abovePayload.size() == kMaxRecoveryPayloadBytes + kMaxLineBytes);
    CHECK(assessText(atPayload).limit == TextLimit::Within);
    CHECK(assessText(abovePayload).limit == TextLimit::RecoveryPayload);
    CHECK(withinTextEnvelope(atPayload) && !withinTextEnvelope(abovePayload));
}
}

int main()
{
    test_support::Suite suite("ResourceLimitsTests");
    suite.run("text around the large-file threshold stays within the envelope", theLargeFileThresholdIsNotATextLimit);
    suite.run("lineCount counts a final line without a newline", lineCountCountsAFinalLineWithoutANewline);
    suite.run("the line limit rejects the next byte", theLineLimitRejectsTheNextByte);
    suite.run("the payload limit rejects the next line", thePayloadLimitRejectsTheNextLine);
    suite.run("empty decision history obeys the entry cap",
              [] { emptyHistoryIsBoundedAndKeepsProtectedRevisions(1, kMaxHistoryEntries + 16); });
    suite.run("decision metadata consumes the byte budget before the entry cap",
              [] { emptyHistoryIsBoundedAndKeepsProtectedRevisions(2 * 1024 * 1024, 140); });
    return suite.report();
}
