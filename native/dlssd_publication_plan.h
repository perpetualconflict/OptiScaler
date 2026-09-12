#pragma once

#include <cstdint>
#include <span>

namespace DlssdPublicationPlan
{
constexpr std::uint32_t kMaxCommandLists = 256;

// CPU-only observation data. The hook supplies opaque D3D12 identities as
// uintptr_t values; no Windows or D3D12 headers are required here.
struct PendingUse
{
    std::uintptr_t commandList = 0;
    std::uint32_t stateBefore = 0;
    bool isBarrierToRead = false;
    bool hasStateBefore = false;
    bool fullUnsplitTransition = false;
};

struct HandleInput
{
    std::uintptr_t producer = 0;
    std::uintptr_t output = 0;
    bool sameListConsumer = false;
    bool unresolved = false;
    std::span<const PendingUse> uses {};
};

struct Selection
{
    std::uint32_t producerIndex = 0;
    std::uint32_t outputStateBefore = 0;
    bool valid = false;
};

inline bool FindUniqueListIndex(std::span<const std::uintptr_t> lists, std::uintptr_t target,
                                std::uint32_t& index)
{
    index = 0;
    if (lists.empty() || lists.size() > kMaxCommandLists || target == 0)
        return false;

    std::uint32_t matches = 0;
    for (std::uint32_t i = 0; i < lists.size(); ++i)
    {
        if (lists[i] == target)
        {
            index = i;
            ++matches;
        }
    }
    return matches == 1;
}

inline bool SelectSingleHandleSuffix(std::span<const std::uintptr_t> lists, const HandleInput& input,
                                     Selection& selection)
{
    selection = {};
    if (lists.empty() || lists.size() > kMaxCommandLists || input.producer == 0 || input.output == 0 ||
        input.sameListConsumer || input.unresolved || input.uses.empty() || input.uses.size() > kMaxCommandLists)
        return false;

    std::uint32_t producerIndex = 0;
    if (!FindUniqueListIndex(lists, input.producer, producerIndex))
        return false;

    std::uint32_t earliestSuffixIndex = kMaxCommandLists;
    const PendingUse* firstUse = nullptr;
    for (const PendingUse& use : input.uses)
    {
        std::uint32_t useIndex = 0;
        if (!FindUniqueListIndex(lists, use.commandList, useIndex) || useIndex <= producerIndex)
            return false;

        if (useIndex < earliestSuffixIndex)
        {
            earliestSuffixIndex = useIndex;
            firstUse = &use;
        }
    }

    if (firstUse == nullptr || !firstUse->isBarrierToRead || !firstUse->hasStateBefore ||
        !firstUse->fullUnsplitTransition)
        return false;

    selection.producerIndex = producerIndex;
    selection.outputStateBefore = firstUse->stateBefore;
    selection.valid = true;
    return true;
}
} // namespace DlssdPublicationPlan
