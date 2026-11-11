// The harness's own contract: what a failure prints, how deferred failures and skips reach the
// suite result, and how the case filter selects cases.
#include "TestHarness.h"

#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using test_support::Suite;

// The message a callable fails with, or nothing when it does not throw Failure.
template <typename Callable> std::string failureOf(Callable callable)
{
    try {
        callable();
    } catch (const test_support::Failure &failure) {
        return failure.what();
    }
    return {};
}

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

// Runs `body` against a fresh suite with its output captured, and restores the filter after.
struct Captured {
    int status = -1;
    std::string out;
    std::string err;
};

template <typename Body> Captured captured(const std::string &filter, Body body)
{
    const auto previous = test_support::options().filter;
    test_support::options().filter = filter;
    std::ostringstream out;
    std::ostringstream err;
    auto *const oldOut = std::cout.rdbuf(out.rdbuf());
    auto *const oldErr = std::cerr.rdbuf(err.rdbuf());
    Captured result;
    try {
        Suite inner("Inner");
        body(inner);
        result.status = inner.report();
    } catch (...) {
        std::cout.rdbuf(oldOut);
        std::cerr.rdbuf(oldErr);
        test_support::options().filter = previous;
        throw;
    }
    std::cout.rdbuf(oldOut);
    std::cerr.rdbuf(oldErr);
    test_support::options().filter = previous;
    result.out = out.str();
    result.err = err.str();
    return result;
}

enum class Colour : unsigned char { Red = 2 };

void aFailedEqualityNamesBothValues()
{
    const int actual = 3;
    const auto message = failureOf([&] { CHECK_EQ(actual, 4); });
    CHECK(contains(message, "actual == 4"));
    CHECK(contains(message, "actual: 3"));
    CHECK(contains(message, "expected: == 4"));
    CHECK(contains(message, "TestHarnessTests.cpp:"));
}

void bytesAreQuotedWithEscapes()
{
    const std::string binary("a\0b\n\xff\"", 6);
    const auto message = failureOf([&] { CHECK_EQ(binary, std::string("a")); });
    CHECK(contains(message, R"("a\0b\n\xff\"")"));
}

void mixedSignednessComparesByValue()
{
    // The built-in == would convert -1 to the largest unsigned value and call them equal.
    CHECK(!failureOf([] { CHECK_EQ(-1, 4294967295u); }).empty());
    CHECK_LT(-1, 1u);
    CHECK_GE(std::size_t{5}, 5);
}

void valuesWithoutStreamOperatorsStillPrint()
{
    CHECK_EQ(test_support::describe(Colour::Red), std::string("enum 2"));
    CHECK_EQ(test_support::describe(std::optional<int>{}), std::string("empty"));
    CHECK_EQ(test_support::describe(std::optional<int>{7}), std::string("7"));
    CHECK_EQ(test_support::describe(std::vector<int>{1, 2}), std::string("[1, 2]"));
    CHECK_EQ(test_support::describe(true), std::string("true"));
    const char *none = nullptr;
    CHECK_EQ(test_support::describe(none), std::string("nullptr"));
}

void scopesLabelTheFailureAndEndWithTheirBlock()
{
    std::string inside;
    {
        SCOPE("row two");
        SCOPE("step b");
        inside = failureOf([] { CHECK(false); });
    }
    CHECK(contains(inside, "[in row two > step b]"));
    CHECK(!contains(failureOf([] { CHECK(false); }), "[in "));
}

void expectRecordsWithoutThrowingUntilTheWaitAsks()
{
    CHECK(!test_support::failurePending());
    CHECK(!EXPECT(1 + 1 == 3));
    CHECK(EXPECT_EQ(2, 2));
    CHECK(test_support::failurePending());
    const auto message = failureOf([] { test_support::throwPending(); });
    CHECK(contains(message, "1 + 1 == 3"));
    CHECK(!test_support::failurePending());
}

void expectFromAnotherThreadReachesTheCase()
{
    std::thread worker([] { EXPECT_EQ(1, 2); });
    worker.join();
    CHECK(test_support::failurePending());
    CHECK(contains(failureOf([] { test_support::throwPending(); }), "actual: 1"));
}

void containRecordsWhatACallbackThrows()
{
    bool returned = false;
    test_support::contain([] { CHECK(2 + 2 == 5); });
    returned = true;
    CHECK(returned);
    CHECK(contains(failureOf([] { test_support::throwPending(); }), "2 + 2 == 5"));
}

void aCaseWithADeferredFailureFails()
{
    const auto result = captured({}, [](Suite &inner) { inner.run("deferred", [] { EXPECT(false); }); });
    CHECK_EQ(result.status, 1);
    CHECK(contains(result.err, "FAIL deferred:"));
}

void aSuiteOfSkippedCasesReportsSkipped()
{
    const auto skipped =
        captured({}, [](Suite &inner) { inner.run("needs root", [] { test_support::skip("runs only as root"); }); });
    CHECK_EQ(skipped.status, 77);
    CHECK(contains(skipped.out, "SKIP needs root: runs only as root"));
    const auto mixed = captured({}, [](Suite &inner) {
        inner.run("passes", [] {});
        inner.run("skips", [] { test_support::skip("not here"); });
    });
    CHECK_EQ(mixed.status, 0);
}

void aSkipAfterAFailedExpectationIsAFailure()
{
    const auto result = captured({}, [](Suite &inner) {
        inner.run("late skip", [] {
            EXPECT(false);
            test_support::skip("too late");
        });
    });
    CHECK_EQ(result.status, 1);
}

void anExceptionOfAnyTypeFailsOnlyItsCase()
{
    const auto result = captured({}, [](Suite &inner) {
        inner.run("throws an int", [] { std::rethrow_exception(std::make_exception_ptr(5)); });
        inner.run("passes", [] {});
    });
    CHECK_EQ(result.status, 1);
    CHECK(contains(result.err, "FAIL throws an int: unknown exception"));
    CHECK(contains(result.out, "PASS passes"));
    CHECK(contains(result.out, "tests=2"));
}

void theFilterSelectsCasesByName()
{
    const auto result = captured("beta", [](Suite &inner) {
        inner.run("alpha case", [] { CHECK(false); });
        inner.run("beta case", [] {});
    });
    CHECK_EQ(result.status, 0);
    CHECK(contains(result.out, "tests=1"));
    CHECK(!contains(result.err, "alpha"));
}

void aFilterThatSelectsNothingFails()
{
    const auto result = captured("no such case", [](Suite &inner) { inner.run("alpha case", [] {}); });
    CHECK_EQ(result.status, 1);
    CHECK(contains(result.err, "matches the filter"));
}

void configureTakesTheHarnessOptionsOutOfTheArguments()
{
    const auto previous = test_support::options();
    std::string program = "tests";
    std::string filter = "--filter";
    std::string value = "name part";
    std::string positional = "/path/to/binary";
    std::vector<char *> argv{program.data(), filter.data(), value.data(), positional.data()};
    const auto remaining = test_support::configure(static_cast<int>(argv.size()), argv.data());
    const auto selected = test_support::options().filter;
    test_support::options() = previous;
    CHECK_EQ(selected, std::string("name part"));
    CHECK_EQ(remaining, (std::vector<std::string>{"tests", "/path/to/binary"}));
}
} // namespace

int main(int argc, char **argv)
{
    test_support::configure(argc, argv);
    Suite suite("TestHarnessTests");
    suite.run("a failed equality names both values", aFailedEqualityNamesBothValues);
    suite.run("bytes are quoted with escapes", bytesAreQuotedWithEscapes);
    suite.run("mixed signedness compares by value", mixedSignednessComparesByValue);
    suite.run("values without stream operators still print", valuesWithoutStreamOperatorsStillPrint);
    suite.run("scopes label the failure and end with their block", scopesLabelTheFailureAndEndWithTheirBlock);
    suite.run("EXPECT records without throwing until the wait asks", expectRecordsWithoutThrowingUntilTheWaitAsks);
    suite.run("EXPECT from another thread reaches the case", expectFromAnotherThreadReachesTheCase);
    suite.run("contain records what a callback throws", containRecordsWhatACallbackThrows);
    suite.run("a case with a deferred failure fails", aCaseWithADeferredFailureFails);
    suite.run("a suite of skipped cases reports skipped", aSuiteOfSkippedCasesReportsSkipped);
    suite.run("a skip after a failed expectation is a failure", aSkipAfterAFailedExpectationIsAFailure);
    suite.run("an exception of any type fails only its case", anExceptionOfAnyTypeFailsOnlyItsCase);
    suite.run("the filter selects cases by name", theFilterSelectsCasesByName);
    suite.run("a filter that selects nothing fails", aFilterThatSelectsNothingFails);
    suite.run("configure takes the harness options out of the arguments",
              configureTakesTheHarnessOptionsOutOfTheArguments);
    return suite.report();
}
