# Matching-frame translated output handoff

`[FSRR] DlssdPresentTranslated=true` requests an experimental, synchronous
queue handoff. It never requests a frozen or delayed snapshot. The owner still
writes only private output; a bridge command list copies the matching result
before the original output-consumer suffix is submitted.

## Receipt and allocation ownership

A version-1 `DlssdFrameTag` joins receipt generation, source NGX evaluation
number, handle, Feature 13 and requested reset. The original runtime frame ABI
is unchanged. All four lease exports are mandatory in the matching sidecar.
The caller appends an immutable GPU generation marker after its staging copies.
Only the exact list's actual ExecuteCommandLists submission can signal that
receipt and schedule the owner. Present and a guessed queue fence are not proof.

The lease retains source resources, staging/HIP allocation ownership, private
output and publication commands through completion. Input list Reset is also
required before reuse, since a recorded list can replay old staging references.
A failed/unknown completion retains the single allocation set; it must not free
memory still reachable by the GPU. On failure the runtime rejects release while
a lease is active. An unretired joint lease is deliberately retained through
proxy unload rather than freeing only its COM or marker references.

## Publication boundary

The trace derives a plan from current per-handle observations. A unique direct
queue producer must precede every observed consumer. The first observation on
the earliest suffix list must be a known, unsplit transition to read. Missing,
duplicate batch identities, same-list consumers, unresolved/overflowed events,
ambiguous handles or incomplete detour installation reject the splice. Repeated
observations on one consumer list are normal and retain recording order.

Execute, Signal, Wait and both tile-mapping queue methods share a bounded,
recursive per-queue gate. Other queues can progress. The handoff submits the
producer prefix, verifies input completion, waits at most nine seconds for the
owner, submits a dedicated output copy, then submits the original suffix.
The observed consumer StateBefore determines the restored destination state;
no fixed batch index or assumed UAV state is used. Copying requires matching
formats, one mip/slice/sample and valid extents/subrects.

A timeout executes the original suffix with its already-recorded FSR-RR output.
A late owner cannot publish. The `publication_submitted` log means commands
were enqueued; `publication_complete=1` and the published counter additionally
require the completion fence. Source-frame gaps reset private history; only
consecutive frames on the same handle retain it. The handoff's own wait does
not count as a new save-load idle gap.

## Validation and limits

The 13-control selector probe, per-queue gate probe and exact input-receipt
probe pass. The five-frame standalone private-output handoff passes with zero
D3D12 debug errors and matching reference hashes. It rejects wrong source/output
frames, premature output, wrong receipts and incomplete retirement flags.
Release x64, shader and profile checks pass. See the workspace document
`docs/DLSSD_MATCHED_FRAME_PUBLICATION_2026-09-12.md` for source/log evidence and
exact candidate hashes.

Game publication and temporal fidelity remain user-run acceptance gates. The
scoped observer does not prove arbitrary unhooked alias, bundle or cross-queue
behavior. Feature 13 already includes reconstruction and upscaling; Feature 1
is independent. The first bounded paired raw capture is pre-engine-postprocessing
and may have reset history; its preview is not the game's display transform.
At the measured approximately 0.8 seconds per owner result, this is a fidelity
experiment, not a performance-ready release.
