// Resource limits: the text envelope at the line, payload and large-file boundaries.
#include "TestHarness.h"
#include "core/ResourceLimits.h"
#include "core/SessionFixtures.h"
#include <string>

using namespace branch_truce;
using test_support::lines;
using test_support::longestLines;

namespace {
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
    return suite.report();
}
