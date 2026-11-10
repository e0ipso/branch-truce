// Allocation fault injection is confined to this executable and the guarded call.
#include "TestHarness.h"
#include "core/SessionFixtures.h"
#include "core/SessionState.h"
#include <algorithm>
#include <cstdlib>
#include <new>

namespace {
thread_local int remaining = -1;
thread_local std::size_t largest = 0;
struct Fault {
    explicit Fault(int after)
    {
        remaining = after;
        largest = 0;
    }
    ~Fault() { remaining = -1; }
};
}

void *operator new(std::size_t bytes)
{
    largest = std::max(largest, bytes);
    if (remaining == 0) throw std::bad_alloc();
    if (remaining > 0) --remaining;
    if (void *memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

using namespace branch_truce;
using namespace test_support;

namespace {
template <typename Operation> void everyAllocationFailureIsAtomic(const FileSession &base, Operation operation)
{
    for (int after = 0; after < 4096; ++after) {
        auto file = base;
        Transition state;
        {
            Fault fault(after);
            state = operation(file);
        }
        if (state != Transition::OutOfMemory) {
            CHECK(after > 0 && (state == Transition::Applied || state == Transition::ConfirmationRequired));
            return;
        }
        CHECK(&file.current() == &base.current());
        CHECK(file.canUndo() == base.canUndo() && file.canRedo() == base.canRedo());
        CHECK(file.publication().checkpoint() == base.publication().checkpoint());
    }
    CHECK(false);
}

void previewAndApplyFailuresAreTyped()
{
    const auto file = batchFile();
    const auto target = choiceTarget(file, SourceSide::Stage3, ChoiceScope::Section).source.contributionId;
    const auto choice = previewChoice(file, file.current().id, target, ChoiceAction::Accept);
    const auto batch = previewNonConflicting(file, file.current().id);
    const auto whole = previewWholeFile(file, file.current().id, SourceSide::Stage3);
    everyAllocationFailureIsAtomic(
        file, [&](auto &copy) { return previewChoice(copy, copy.current().id, target, ChoiceAction::Accept).state; });
    everyAllocationFailureIsAtomic(file,
                                   [&](auto &copy) { return previewNonConflicting(copy, copy.current().id).state; });
    everyAllocationFailureIsAtomic(
        file, [&](auto &copy) { return previewWholeFile(copy, copy.current().id, SourceSide::Stage3).state; });
    everyAllocationFailureIsAtomic(file, [&](auto &copy) { return applyChoice(copy, choice, true); });
    everyAllocationFailureIsAtomic(file, [&](auto &copy) { return applyNonConflicting(copy, batch, true); });
    everyAllocationFailureIsAtomic(file, [&](auto &copy) { return applyWholeFile(copy, whole, true); });
    const ResultEdit edit{{0, 0}, "typed\n"};
    everyAllocationFailureIsAtomic(file, [&](auto &copy) { return copy.editResult(copy.current().id, edit); });
    auto edited = file;
    CHECK(edited.editResult(edited.current().id, edit) == Transition::Applied);
    everyAllocationFailureIsAtomic(edited, [&](auto &copy) { return copy.undo(copy.current().id); });
    CHECK(edited.undo(edited.current().id) == Transition::Applied);
    everyAllocationFailureIsAtomic(edited, [&](auto &copy) { return copy.redo(copy.current().id); });
    bool defect = false;
    try {
        withinMemory([]() -> Transition { throw std::logic_error("deliberate defect"); });
    } catch (const std::logic_error &) {
        defect = true;
    }
    CHECK(defect);
}

void oversizedEditsAreRejectedBeforeCopyingText()
{
    auto file = choiceFile();
    const auto &before = file.current();
    const ResultEdit huge{{0, before.draft.text.size()}, std::string(kMaxRecoveryPayloadBytes + 1, '\n')};
    const EditingCommand command{before.id, {}, {huge}, {}, EditingScope::WholeFile};
    for (const bool mapped : {false, true}) {
        Transition state;
        std::size_t allocation;
        {
            Fault fault(-1);
            state = mapped ? file.applyCommand(command, true) : file.editResult(before.id, huge);
            allocation = largest;
        }
        CHECK(state == Transition::TextTooLarge && allocation < kMaxRecoveryPayloadBytes);
        CHECK(&file.current() == &before && !file.canUndo());
    }
    const auto text = longestLines(kMaxRecoveryPayloadBytes / kMaxLineBytes);
    FileSession full("full", "full.txt", {text, {}, 0});
    const ResultEdit extra{{0, 0}, "\n"};
    Transition state;
    {
        Fault fault(0);
        state = full.editResult(full.current().id, extra);
    }
    CHECK(state == Transition::TextTooLarge && full.current().draft.text == text);
}
}

int main()
{
    Suite suite("AllocationFailureTests");
    suite.run("preview and apply allocation failures are typed and atomic", previewAndApplyFailuresAreTyped);
    suite.run("oversized edits are refused before copying text", oversizedEditsAreRejectedBeforeCopyingText);
    return suite.report();
}
