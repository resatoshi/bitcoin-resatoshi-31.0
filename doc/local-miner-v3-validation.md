# Local Windows miner 31.0.3 validation

Date: 2026-09-11. Branch: `fix/windows-miner-recycle-upgrade`.
No upload, installation over an existing installation, or mainnet deployment
is part of this change.

## Scope

Recycle consensus, historical blocks and wallet units are unchanged in this
simplification. Existing shared recycle fixes remain in both local branches.

- Bootstrap handoff requires completed initial synchronization and three
  non-bootstrap block-serving connections, each eligible for 60 seconds.
- Missing statistics under the main validation lock skip an observation.
- CPU mining and its start button require a completed relay connection.
- Core's added-node thread owns connection retries and DNS resolution. The
  GUI has no network future/thread to join when its manager is destroyed.
- Base-seed IP resolutions are reused from Core, with a bounded latest
  resolution per configured seed. Hostnames and port come from chain parameters.
- Only manager-owned registrations are removed; user addnodes are preserved.
  Explicit `-connect` configurations take precedence over automatic discovery.
- Zero peers requests registration immediately; actual attempts follow Core's
  existing background schedule. Before handoff Core also retries with 1–2 peers.
- Test executables are excluded from the installer and release packaging step.

Core can still wait for its own OS resolver during shutdown. This change removes
extra GUI network workers; it does not promise a universal shutdown deadline.

## Validation

Native Windows Qt suites passed 27 results, including setup/cleanup, with no
failures. New cases cover unfinished handshakes, IBD before handoff, skipped
statistics under lock contention, Core seed-address caching, zero-peer recovery,
network disable/enable, and preservation of user-added connection entries.

A public mainnet block-1 fixture verifies that a height-1 node with a two-day-old
tip stays in IBD and the CPU miner remains paused despite a connected peer.
Making the tip recent with a test clock allows IBD completion and mining.
This is a reproduction of a remaining operator recovery condition, not an
automatic workaround. See the Windows miner guide before network-wide rollout.

Targeted native Windows network/recycle unit suites passed 27 cases and
145,598 assertions. Unselected suites were not run in this pass.

Linux functional tests `feature_recycle.py`, `feature_recycle_spend.py`, and
`rpc_generate.py` passed on the 31.0.3 build. Final network/recycle unit results,
artifact hashes and build commit are recorded under the ignored `build-audit/`
directory and alongside the distributables.

Both the main-fix and miner binaries passed an isolated recovery exercise:
import public mainnet block 1, create a wallet, stop, remove the derived recycle
schedule/version marker, make the original block bytes unavailable, and start.
Startup safely refused repair. Restoring the block file allowed automatic
schedule repair, preserved the block hash and wallet ownership, and passed
`verifychain 4 0`. This simulates unavailable original data; it is not an
end-to-end pruning test of a long chain or every legacy corruption pattern.

Initial development failures included an unavailable cross-toolchain PATH,
a stale object after an interface edit, and incorrect test setup for RPC
registration and the trusted fixture's minimum-work precheck. These were
resolved before final validation. Bootstrap registration now calls Core's
connection list directly without depending on RPC initialization.

Temporary nodes, wallets and helper tools are removed after validation. Useful
sources, build products and result logs are retained. Earlier full-suite and
Windows wallet migration results remain documented in the 31.0.2 report; they
are historical results, not additional full-suite executions for this build.
Remote CI and its full platform matrix remain unexecuted.
