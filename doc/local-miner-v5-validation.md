# Local Windows miner 31.0.5 security fixes

Scope: two peer-trust findings from the review of 31.0.4. Main branch
`fix/recycle-state-integrity` remains at
`69c9cf115efb1824805e7216741e76029cdc464e`.

## Behavior

- Old-tip CPU mining no longer treats VERSION starting heights or unvalidated
  header presync as proof of a better chain. Validated best-header work, missing
  block bodies, known peer headers and in-flight blocks still gate readiness.
  A peer must still confirm the old tip above genesis.
- Bootstrap retirement requires three automatically selected outbound block
  peers in distinct routable network groups, each stable for 60 seconds after
  synchronization. Inbound and manual connections cannot qualify. Existing
  Core network grouping is reused without an AS map; distinct groups do not
  prove distinct operators or prevent every eclipse attack.
- With no bootstrap or qualifying outbound peer left, inbound-only connections
  no longer suppress bootstrap recovery. Retry throttling, explicit `-connect`,
  network-disable behavior and preservation of user addnodes remain covered.

Consensus, difficulty, recycle accounting and Core's global IBD policy are
unchanged by these fixes. A small network without three qualifying groups will
retain bootstrap connections longer. This is intentional.

## Validation

Windows Qt tests passed: 27 results including setup/cleanup, zero failures.
The regression tests exercise real in-process peer message handling with no
external sockets: an inbound VERSION advertising INT32_MAX and no block service
cannot pause mining on a validated five-day-old tip. Missing bodies and absent
header confirmation still prevent mining. Three inbound peers and duplicate
outbound IPv4 network groups cannot retire bootstrap. A third independent
outbound group earns its own 60-second interval, and inbound-only recovery works.

Build and package logs are retained under ignored `build-audit/security-fixes-*`.
No operational node or real wallet is used, and remote CI/deployment is not run.

## Minimal-fork assessment

Baseline is local commit `0bb4cdfb0f` (Import Bitcoin Core 31.0 source), not an
independently authenticated upstream release comparison. At the start of this
change the main branch differs in 91 files. Excluding tests and documentation,
its `src/` implementation changes cover 41 files, 829 added and 137 removed lines.
The large total deletion count mostly removes Bitcoin-specific test fixtures.

The original cryptographic and script directories are unchanged. Recycle hooks
are concentrated around coin state, block connection/disconnection, reward,
indexing and wallet availability. GUI mining necessarily adds more code on the
miner branch. The project substantially preserves Core's implementation, but
recycle and ASERT are significant consensus changes regardless of line count.
Keep future features separate from these narrow consensus hooks; do not reduce
necessary validation, undo handling or tests merely to reduce the patch size.
