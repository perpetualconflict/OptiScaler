# Publication prerequisite: exclusive staging reservation

The September 8 game log validated 16 sparse owner evaluations, but those successes do not prove pending-job lifetime under a delayed submission. Source review found RunPrepareCreateThenQueue could record new copies before replacing gPending; NotifyFrameSubmitted also cleared gHavePending before queueing the owner. A second evaluate could overwrite the same staging textures during that transfer gap.

Reserve the single staging set under gJobMutex before recording copies. Reject a new job while pending, queued, or owner-busy. Transfer gPending directly to gJob under the same lock, preserving its resource references. This addresses the CPU ownership race; it does not establish GPU submission completion or make reset/drop retirement safe.

Evidence: workspace docs/windows-win32-direct3d12.pdf, page 294 (Fence-Based Resource Management), explains that resource reuse must follow GPU progress. Page 291 requires a fence before CPU readback. Streamline/source/plugins/sl.dlss_d/dlss_dEntry.cpp supplies the input-read/output-UAV boundary. These support preserving the existing copy barriers and completion checks, not enabling late game-output writes.

Validation: Release x64 build, existing five-frame isolated sidecar test, code review of pending-to-owner ownership under one mutex. The isolated test does not exercise concurrent game callbacks. User game regression remains required. Exact input-list submission tickets, GPU-safe reset/drop, and immutable output handoff remain publication gates.
