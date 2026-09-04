globallycoherent RWByteAddressBuffer Shared : register(u0);

cbuffer Constants : register(b0)
{
    uint BaseByte;
    uint Sequence;
    uint MaxWaitIterations;
};

static const uint ReadyOffset = 0;
static const uint DoneOffset = 4;
static const uint HipStatusOffset = 8;
static const uint D3dStatusOffset = 12;
static const uint PayloadOffset = 16;
static const uint D3dObservedOffset = 20;
static const uint HipWaitIterationsOffset = 24;
static const uint D3dWaitIterationsOffset = 28;
static const uint SequenceMirrorOffset = 32;

[numthreads(1, 1, 1)]
void SignalReadyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint ignored;
    Shared.Store(BaseByte + DoneOffset, 0);
    Shared.Store(BaseByte + HipStatusOffset, 0);
    Shared.Store(BaseByte + D3dStatusOffset, 0);
    Shared.Store(BaseByte + PayloadOffset, 0);
    Shared.Store(BaseByte + D3dObservedOffset, 0);
    Shared.Store(BaseByte + HipWaitIterationsOffset, 0);
    Shared.Store(BaseByte + D3dWaitIterationsOffset, 0);
    Shared.Store(BaseByte + SequenceMirrorOffset, 0);
    DeviceMemoryBarrier();
    Shared.InterlockedExchange(BaseByte + ReadyOffset, Sequence, ignored);
}

[numthreads(1, 1, 1)]
void WaitDoneCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint observed = 0;
    uint waitIterations = 0;
    for (; waitIterations < MaxWaitIterations; ++waitIterations)
    {
        Shared.InterlockedAdd(BaseByte + DoneOffset, 0, observed);
        if (observed == Sequence)
            break;
    }

    Shared.Store(BaseByte + D3dObservedOffset, observed);
    Shared.Store(BaseByte + D3dWaitIterationsOffset, waitIterations);
    Shared.Store(BaseByte + D3dStatusOffset, observed == Sequence ? 1 : 2);
    DeviceMemoryBarrier();
}
