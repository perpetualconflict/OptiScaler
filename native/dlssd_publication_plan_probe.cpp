#include "dlssd_publication_plan.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

namespace
{
using DlssdPublicationPlan::HandleInput;
using DlssdPublicationPlan::PendingUse;
using DlssdPublicationPlan::Selection;

constexpr std::uintptr_t kOutput = 0x9000;

bool TestPositiveOutOfRecordingOrder()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 2> uses {
        PendingUse { lists[5], 55, true, true, true },
        PendingUse { lists[4], 44, true, true, true },
    };
    Selection selection {};
    const HandleInput input { lists[3], kOutput, false, false, uses };
    return DlssdPublicationPlan::SelectSingleHandleSuffix(lists, input, selection) && selection.valid &&
           selection.producerIndex == 3 && selection.outputStateBefore == 44;
}

HandleInput ValidInput(const std::array<std::uintptr_t, 7>& lists, const std::array<PendingUse, 1>& uses)
{
    return HandleInput { lists[3], kOutput, false, false, uses };
}

bool TestRejectDuplicateProducer()
{
    const std::array<std::uintptr_t, 5> lists { 0x10, 0x20, 0x30, 0x40, 0x40 };
    const std::array<PendingUse, 1> uses { PendingUse { lists[4], 1, true, true, true } };
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, false, uses }, selection);
}

bool TestRejectMissingProducer()
{
    const std::array<std::uintptr_t, 5> lists { 0x10, 0x20, 0x30, 0x40, 0x50 };
    const std::array<PendingUse, 1> uses { PendingUse { lists[4], 1, true, true, true } };
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { 0x999, kOutput, false, false, uses }, selection);
}

bool TestRejectDuplicateConsumerBatchList()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x50, 0x70 };
    const std::array<PendingUse, 1> uses { PendingUse { lists[4], 1, true, true, true } };
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, false, uses }, selection);
}

bool TestAcceptRepeatedObservationsOnConsumer()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 2> uses {
        PendingUse { lists[5], 1, true, true, true },
        PendingUse { lists[5], 1, true, true, true },
    };
    Selection selection {};
    return DlssdPublicationPlan::SelectSingleHandleSuffix(
               lists, HandleInput { lists[3], kOutput, false, false, uses }, selection) &&
           selection.producerIndex == 3 && selection.outputStateBefore == 1;
}

bool TestRejectMissingConsumer()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 0> uses {};
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, false, uses }, selection);
}

bool TestRejectMissingConsumerList()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 1> uses { PendingUse { 0x999, 1, true, true, true } };
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, false, uses }, selection);
}

bool TestRejectConsumerBeforeProducer()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 1> uses { PendingUse { lists[2], 1, true, true, true } };
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, false, uses }, selection);
}

bool TestRejectSameListOrUnresolved()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 1> uses { PendingUse { lists[4], 1, true, true, true } };
    Selection selection {};
    const bool sameList = !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, true, false, uses }, selection);
    const bool unresolved = !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, true, uses }, selection);
    return sameList && unresolved;
}

bool TestRejectWrongFirstEvent()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 2> uses {
        PendingUse { lists[5], 1, true, true, true },
        PendingUse { lists[4], 1, false, true, true },
    };
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, false, uses }, selection);
}

bool TestRejectWrongFirstEventSameConsumer()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 2> uses {
        PendingUse { lists[4], 1, false, true, true },
        PendingUse { lists[4], 1, true, true, true },
    };
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, false, uses }, selection);
}

bool TestRejectOverflowCount()
{
    std::array<std::uintptr_t, DlssdPublicationPlan::kMaxCommandLists + 1> lists {};
    for (std::size_t i = 0; i < lists.size(); ++i)
        lists[i] = static_cast<std::uintptr_t>(i + 1);
    const std::array<PendingUse, 1> uses { PendingUse { lists.back(), 1, true, true, true } };
    Selection selection {};
    return !DlssdPublicationPlan::SelectSingleHandleSuffix(
        lists, HandleInput { lists[3], kOutput, false, false, uses }, selection);
}

bool AcceptAtMostOneCandidate(std::span<const std::uintptr_t> lists, std::span<const HandleInput> candidates)
{
    bool found = false;
    Selection selected {};
    for (const HandleInput& candidate : candidates)
    {
        Selection current {};
        if (!DlssdPublicationPlan::SelectSingleHandleSuffix(lists, candidate, current))
            continue;
        if (found)
            return false;
        found = true;
        selected = current;
    }
    return found && selected.valid;
}

bool TestRejectDuplicatedCandidateHandledOuter()
{
    const std::array<std::uintptr_t, 7> lists { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 };
    const std::array<PendingUse, 1> firstUses { PendingUse { lists[4], 1, true, true, true } };
    const std::array<PendingUse, 1> secondUses { PendingUse { lists[5], 2, true, true, true } };
    const std::array<HandleInput, 2> candidates {
        ValidInput(lists, firstUses),
        HandleInput { lists[3], 0xA000, false, false, secondUses },
    };
    // The per-handle helper accepts each candidate; the publication hook's
    // outer candidate count must reject the ambiguous pair.
    return !AcceptAtMostOneCandidate(lists, candidates);
}

template <typename Test>
bool Run(const char* name, Test test)
{
    const bool passed = test();
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}
} // namespace

int main()
{
    const bool positive = Run("positive out-of-recording-order consumer selection", TestPositiveOutOfRecordingOrder);
    const bool duplicateProducer = Run("duplicate producer", TestRejectDuplicateProducer);
    const bool missingProducer = Run("missing producer", TestRejectMissingProducer);
    const bool duplicateConsumerBatchList =
        Run("duplicate consumer list in batch", TestRejectDuplicateConsumerBatchList);
    const bool repeatedObservations =
        Run("repeated observations on one consumer list", TestAcceptRepeatedObservationsOnConsumer);
    const bool missingConsumer = Run("missing consumer", TestRejectMissingConsumer);
    const bool missingConsumerList = Run("missing consumer list", TestRejectMissingConsumerList);
    const bool beforeProducer = Run("consumer at or before producer", TestRejectConsumerBeforeProducer);
    const bool sameListOrUnresolved = Run("same-list and unresolved flags", TestRejectSameListOrUnresolved);
    const bool wrongFirstEvent = Run("wrong first event", TestRejectWrongFirstEvent);
    const bool wrongFirstEventSameConsumer =
        Run("wrong first event on repeated consumer observations", TestRejectWrongFirstEventSameConsumer);
    const bool overflow = Run("command-list count overflow", TestRejectOverflowCount);
    const bool duplicateCandidate =
        Run("duplicated candidate handled by outer selector", TestRejectDuplicatedCandidateHandledOuter);
    const bool passed = positive && duplicateProducer && missingProducer && duplicateConsumerBatchList &&
                        repeatedObservations && missingConsumer && missingConsumerList && beforeProducer &&
                        sameListOrUnresolved && wrongFirstEvent && wrongFirstEventSameConsumer && overflow &&
                        duplicateCandidate;
    return passed ? 0 : 1;
}
