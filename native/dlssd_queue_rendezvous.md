# Queue rendezvous workload ABI

This opt-in diagnostic exports `DLSSD_RENDEZVOUS_ExecuteWorkload`; loader types
and the caller contract are in `dlssd_queue_rendezvous_abi.h`. The legacy
`Execute` export uses the same path with a deterministic synthetic payload.
It is not translated DLSS-D execution and imports no model or image resources.

The host enqueues an arm marker/event, ready-wait, callback work, and completion
on the legacy default HIP stream. After the arm event completes, the caller
submits the D3D12 ready/wait tail, synchronizes HIP, and uses a D3D12 fence to
retire the slot. Preserve external serialization and unique sequence ownership.
The helper selects the token's matched adapter on execution and cleanup.

A callback must enqueue promptly, preallocate/prewarm its work, and retain GPU
resources until both APIs finish. A nonzero return or C++ exception queues
failure publication behind partial work and disables further enqueues on the
token. Still submit the tail and drain; a return error never means nothing was
queued. Read thread-local errors before calling another helper function.
The completion status is 3 for callback failure, or 2 if readiness timed out.
Unrecoverable launch/device errors may prevent publication; the D3D12 poll is
still bounded. Destroy retains the existing cleanup-error reporting.

Ready timeout does not cancel arbitrary queued kernels. Only bounded scratch
work that is safe without D3D12 readiness is authorized. Real resources require
a validated device-side readiness guard and recovery strategy. A callback that
hangs or queues an infinite kernel violates the ABI and cannot be rescued by
the helper. Default-stream/device-wide synchronization is retained for this
regression; stream ownership/performance changes are separate experiments.

Validation: `scripts/build-release.ps1` completed with zero errors. Five fresh
processes of `scripts/test-dlssd-queue-rendezvous.ps1` / its compiled probe each
passed 32 legacy CPU-oracle exchanges, callback scratch-write success, explicit
failure and exception after partial work, two poisoned-token reuse checks, and
one intentional bounded timeout. Scratch readback, device-removal status and
cleanup passed. An earlier candidate failed the first exchange; explicit idle
protocol-buffer initialization was added before the final five passes. The
original failure cause is not proven; manual game regression is still required.

The preceding `fe62eab9` Cyberpunk run passed 4,892/4,892 exchanges without any
scheduling/coherency failure. Its shutdown-only HIP 100 was a cleanup warning.
That validates the old synthetic game protocol, not this split-kernel revision.
After a clean manual regression, connect one already translated scratch kernel
in the isolated investigation before revalidating the 310.7 typed resource chain.
