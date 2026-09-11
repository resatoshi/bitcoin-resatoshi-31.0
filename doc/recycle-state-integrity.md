# Local recycle state integrity changes

This work is based on main commit `dbb985d0ab039a9a9d3c00147d133d84c44f2385`.
It is a local implementation and validation result, not a mainnet deployment.

## Rules retained

- Mainnet expiry remains 5,256,000 blocks after an output is created. This is
  approximately 100 years at ten minutes per block, not a wall-clock deadline.
- Outputs spent in their expiry block are not recycled.
- Expired unspent value enters the pool. Coinbase may claim up to the lesser of
  one coin and the available pool, in addition to subsidy and transaction fees.
- A partial recycle claim leaves the remainder in the pool; an empty pool pays
  no recycle reward. Recycling transfers existing value rather than issuing
  additional subsidy.
- The 200-block lifetime is available only with `-test=recycle` on regtest.

## Implementation

Internal recycle records retain their existing serialized bytes. A dedicated
reader prevents the ordinary transaction-script size limit from truncating
large schedules and undo records. Actual transaction rules are unchanged.
Disconnecting the first expiry restores the absence of the pool record as well
as the original UTXOs. Local state errors are distinguished from invalid blocks.

Coin statistics account for expired outputs and claimed recycle rewards, using
`indexes/coinstatsindex-recycle-v1` when recycling is enabled. The old index is
not reused. Wallet balances and coin selection exclude expired outputs and
restore them when a reorg makes them available again. `listunspent` reports
expiry height and remaining blocks. Block filters and REST spent-output results
exclude the synthetic recycle undo entry.

## Existing data: rollout verification deferred

The implementation includes an initial schedule audit and reconstruction from
original blocks for chainstates without the new codec marker. It repairs derived
state; it does not rewrite historical blocks. Missing pruned source blocks can
prevent repair. A codec marker prevents older binaries from opening an upgraded
chainstate; binary downgrade is therefore not a supported rollback procedure.

An operational migration rehearsal, including damaged legacy databases,
pruning, interruption during the initial audit, and rollback procedures, remains
deferred at the user's request. This code must not be treated as approved for
production rollout on the strength of local CI checks alone.

## Local validation

- Release build on Linux with GCC 15.2; GUI and IPC disabled.
- Full unit suite: 743 executed tests passed, 5 skipped; 26,979,223 assertions
  passed. One passing test emitted a warning.
- Functional coverage: `feature_recycle.py`, `mining_mainnet.py`,
  `feature_coinstatsindex.py`, `feature_utxo_set_hash.py`, `rpc_getblockstats.py`,
  `wallet_balance.py`, `feature_reindex.py`, and `interface_rest.py`.
- Recycle coverage includes large records on disk, undo serialization, expiry,
  wallet restart, reorg, partial claims, pool depletion, index/scan equality,
  and recovery after a deliberately interrupted chainstate flush.
- A separate temporary node downloaded mainnet blocks. At height 1,241 its
  block hash, UTXO count (1,241), amount (62,050), and MuHash matched the running
  node. `verifychain 4 0` passed on the temporary node. This is a pre-expiry
  compatibility check, not evidence of a completed legacy-state migration.
- Python lint uses CI's Python 3.10.14, Ruff 0.15.5, mypy 1.19.1 and Vulture
  2.14. Type checks, selected Ruff rules and dead-code checks passed.
- File modes, includes, include guards, circular dependencies, test conventions,
  argument documentation, translations and locale checks passed locally.

GitHub Actions, other operating systems, sanitizers and clang-tidy jobs have not
been run. No production node was stopped and no changes were pushed.

## Repeated review and validation, 2026-09-11

The production changes were reviewed again, including serialization bounds,
expiry-block spending, fee/recycle separation, index rollback, wallet depth
handling, mempool removal, and initial database audit/version handling. No new
production-code defect was reproduced in this pass; production code was not
changed. This is not a claim that all defects have been excluded.

- The full unit suite passed three times with Boost randomized ordering seeds
  17, 271 and 65537. Each run reported 742 passing cases, one warning-only case
  and five skipped cases. The warning-only case lacked external script assets.
- Added unit cases for oversized/truncated metadata, oversized non-metadata
  scripts, and missing expiry schedules. The final full run, seed 20260911,
  reported 744 passing cases, the same warning-only case and five skips;
  all 26,916,812 assertions passed.
- Downloaded the same external script fixture used by CI and ran the omitted
  `script_assets_tests` separately: 141,917 assertions passed without warnings.
  Fixture SHA256:
  `cd789a58ec45916e1721cdd14e82ca4c93100959f1cef4e229b22e3bf539f095`.
- Added `feature_recycle_spend.py`: two pending spends at the expiry boundary,
  template fee accounting, rejection of a one-satoshi excess reward without
  changing chainstate, inclusion of one spend and expiry of the other output,
  mempool eviction, wallet balance, index/scan equality, reorg and restart.
- All nine functional tests (the previous eight plus the new spend test)
  passed in each of three runs with seeds 17, 271 and 65537: 27 passing test
  executions. The new spend test also passed a standalone development run.
- The final build, mypy (313 files), CI-selected Ruff rules, Vulture, file modes,
  test conventions, includes, include guards, circular-dependency checks and
  `git diff --check` passed.

Temporary node data, unit-test directories and downloaded fixtures were removed
after use. Result logs remain under `build/recheck-*.log`. The earlier limits on
Windows/GUI, remote CI, sanitizers and legacy-data migration validation still
apply. No mainnet deployment or historical-block modification was performed.


## Final local recovery check, 2026-09-11

Both the main-fix and miner-upgrade Linux binaries were tested with isolated
mainnet block-1 data and a disposable wallet. Removing the derived schedule
and codec marker, then making original block bytes unavailable, caused startup
to refuse repair. Restoring the original file allowed schedule reconstruction,
retained the best block and wallet ownership, and passed `verifychain 4 0`.
This simulates unavailable source data, not a full pruning lifecycle test.
No operating node or real wallet was modified.
