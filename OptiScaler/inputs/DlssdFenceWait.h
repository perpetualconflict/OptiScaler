#pragma once
#include <cstdint>
#include <Windows.h>
#include <d3d12.h>

// The event is a wakeup hint, never proof of completion. A timed-out earlier
// registration can wake the reused event while a newer fence value is pending.
namespace DlssdFenceWait {
template<class Fence>
bool Complete(Fence* fence, UINT64 target, HANDLE event, DWORD timeoutMs)
{
    if (!fence || !event || target == UINT64_MAX || timeoutMs == INFINITE)
        return false;
    const ULONGLONG start = GetTickCount64();
    auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return false; // Device removed, not completed.
    if (completed >= target) return true;
    if (timeoutMs == 0 || FAILED(fence->SetEventOnCompletion(target, event))) return false;
    for (;;)
    {
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= timeoutMs) return false;
        const DWORD result = WaitForSingleObject(event, static_cast<DWORD>(timeoutMs - elapsed));
        completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return false;
        if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) return false;
        if (completed >= target) return true;
        if (result == WAIT_TIMEOUT) return false;
    }
}
}
