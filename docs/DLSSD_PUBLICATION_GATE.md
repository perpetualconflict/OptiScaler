# Translated output publication gate

The 2026-09-08 Cyberpunk run attached successfully but never published:
owner_ok=3, owner_fail=1, published=0. Three output sample failures were
temporarily excused as warmup; the fourth sample reported 768 nonfinite RGB
components in a 16x16 region after successful private output copy-back.

The owner queue wait proves only that earlier input copies completed. It does
not reserve the captured game output during a later private evaluation, or
order the game's consumers after a private output copy. Meanwhile the NGX path
continues producing FSR-RR each frame. Streamline's scoped output UAV state is
valid on the evaluate caller list, not indefinitely on an asynchronous owner.

`DlssdExperimentalBackend` therefore always requests private owner evaluation.
`DlssdPresentTranslated` remains a saved preference but logs that caller-list
output handoff is unavailable. Successful owner evaluations still count as
diagnostic execution; `published` remains zero. No per-host warmup count is
used to establish output validity.

Owner evaluations are sparse diagnostics, normally separated by two seconds.
Game motion vectors relate adjacent game frames, so they cannot connect those
private samples to the private backend's previous history. Every owner job now
forces its private snapshot's reset to one and logs requested/effective reset.
This also resets the first private evaluation after attach. The caller's
snapshot and game behavior are unchanged; the standalone consecutive-frame
history probe retains its own reset policy. This corrects history continuity
semantics but is not evidence that missing reset caused the observed NaNs.

Before removing the gate, provide an owned completion resource and fence,
generation and resolution matching, output lifetime protection, correct
subrect/state handling on the current NGX caller list, and fallback suppression
only when that list actually records an accepted publication. A serialized
standalone output copy does not validate game publication ownership.

Validation: Release x64 build and profile/shader checks pass. The current game
run predates this gate; a subsequent game run is user-owned. Sidecar numeric
validation and texture dataflow remain independently testable in Investigation.
