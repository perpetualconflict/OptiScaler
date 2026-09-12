# Exact input submission prerequisite

The September 12 game capture contains a recognizable scene, but its manifest
correctly leaves game-frame identity unverified. The backend currently saves
`State::currentCommandQueue` with its pending input copies and lets Present
release the owner job. Signalling that queue later does not identify the
submission containing those copies.

`native/dlssd_input_receipt.h` is an isolated, single-recording prototype for
replacing that assumption. It is **not included in the game backend yet**.
It appends an immutable 64-bit generation marker copy to the input-copy list,
matches that list in an actual submission, and signals a private fence after
the real ExecuteCommandLists call. A successful fence plus matching readback
marker establishes execution of that recording. A signal without execution
fails the marker check.

The helper keeps its marker resources, list and queue referenced. Successful
Reset before submission cancels the recording. Reset after submission does not
permit retirement until GPU completion is also verified. Timeout permits retry;
unknown completion retains the receipt. Destruction before safe retirement
deliberately retains its GPU objects/event. That diagnostic failure behavior is
not a replacement for the staging and HIP resource lease implementation.

## Integration contract

- Serialize helper methods. Do not hold a game-facing lock over a blocking wait.
- Normalize Streamline wrapper identity before recording, to match the pointer
  seen by the actual Execute hook; retain the resulting COM interface.
- Reserve the complete input staging set before recording copies and append the
  marker after those copies. Keep scalar/frame metadata with that same generation.
- Invoke BeforeSubmit and AfterSubmit around the same real Execute call. Never
  substitute Present, a guessed queue, or a fence on an unrelated queue.
- Observe successful command-list Reset, not allocator Reset. Retain in-flight
  allocations across reset. A replay of the same unreset recording is unsupported
  and must retain/poison the diagnostic rather than silently reuse a receipt.
- Retain game copy sources, staging allocations, descriptor translations and HIP
  tokens independently until their last GPU/owner use. COM references alone do
  not retain a released HIP mapping.
- Do not enable publication from this receipt alone. Output consumers can already
  be inside the same submitted list/batch. A consumer-ordered handoff and immutable
  result ownership are separate requirements.

## Validation

Build with `scripts/build-input-receipt-probe.ps1`, then run
`native/out/input-receipt/dlssd_input_receipt_probe.exe`.
The September 12 AMD hardware run passed with `/W4 /WX`, no warnings, and
`D3D_DEBUG errors=0 discarded=0`. It covers exact list index and actual queue,
unrelated/duplicate list rejection, wrong signal queue, reset-before-submit and
pointer reuse, signal-without-execution, and reset-in-flight with timeout/retry.
The delayed test uses a fresh allocator for Reset while retaining the in-flight
allocator, then releases its queue blocker before evaluating the assertion.

Evidence: workspace `runtime-staging/input-receipt-20260912/`.
No game, NGX, or HIP process is used by this probe. These tests validate the
receipt mechanism, not integration, image fidelity, or game publication.

## Source basis

Local `docs/windows-win32-direct3d12.pdf`: pages 49/56 distinguish recording
from submission; page 291 requires completed readback before CPU access; page
294 describes GPU-progress requirements before reuse. Matching Streamline 2.12
records DLSSD transitions and NGX evaluation on the supplied list in
`Streamline/source/plugins/sl.dlss_d/dlss_dEntry.cpp` (802–855, 1048).

The existing hook is `DlssdOutputHazardTrace::hkExecuteCommandLists`; it has
no Reset hook today. The current input state mapping was checked against
Streamline's D3D12 implementation: eTextureRead maps to PIXEL_SHADER_RESOURCE,
and eStorageRW maps to UNORDERED_ACCESS. No state-mapping defect was established.
