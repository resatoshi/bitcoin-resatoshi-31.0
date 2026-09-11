# Local Windows miner 31.0.2 validation

Historical results. Superseded by [31.0.3 validation](local-miner-v3-validation.md).

Date: 2026-09-11. Branch: `fix/windows-miner-recycle-upgrade`.
This records the final replacement for the earlier local 31.0.1 miner.
Source changes remain local and uncommitted; nothing was pushed or deployed.

## Recovery after the unexpected shutdown

Git object verification (`git fsck --full --no-reflogs`) reported no corrupt
objects. Four dangling blobs were preserved because they can contain earlier
work; they are not corruption. Changed Python files parsed, changed files were
not unexpectedly empty, and whitespace checks passed. Both builds completed
and were checked again after restart. No source rollback was necessary.

The original worktree's uncommitted `rpc_generate.py` changes were preserved.
The existing production node responded at height 1,312 with matching headers,
IBD false and no warnings. Read-only `verifychain 4 0` returned true. The node
was not replaced or stopped by this work. This does not diagnose the cause of
the computer shutdown or certify every file on the computer.

## Final changes

- CPU workers use Core's local mining interface, with a unique coinbase
  extranonce for each template across workers, refreshes and start/stop cycles.
- Templates are reused for up to one second. Workers check stop requests
  during hashing and reconsider the parent tip/network state every 4,096
  hashes. The dashboard requests stop asynchronously instead of joining a
  long RPC batch on the GUI thread.
- Network loss or incomplete synchronization pauses a running miner; becoming
  ready resumes it. Stop cancels resumption. Above genesis, Core IBD completion
  is required; a very old chain tip can therefore keep a restarted GUI paused.
- Actual hash attempts, including successful attempts, are divided by elapsed
  monotonic time. SHA256 midstate reuse saves repeated header work.
- The optional mining RPC startnonce remains supported. UINT32_MAX is now
  tested and a solution on the last permitted attempt is accepted.
- Bootstrap management runs once per GUI node, not once per wallet. It uses
  hostname/IP identification, completed handshakes and block-serving services.
  Three non-bootstrap peers must each remain eligible for 60 seconds before
  bootstrap connections are disconnected. A changed peer starts a new interval.
  Remaining connections are rechecked before disconnecting; nodes are not banned.
- Zero total peers triggers bootstrap attempts immediately, with 60-second
  retries while empty. One or two peers do not trigger this explicit fallback.
  Disabled networking suppresses attempts. Reappearing bootstraps are retired
  again while three stable peers remain. Core's normal peer discovery continues.
- DNS/connect work stays off the GUI thread. Failed DNS resolution delays
  handoff; configured name proxies are not bypassed with local DNS queries.

These miner changes do not alter BTC/sat units, addresses, block formats or
mainnet consensus rules. Previously integrated recycle state/wallet fixes remain.

## Executed validation

Final native Windows 11 Qt suites: **26 results passed**, including setup and
cleanup results, with no failures. Tests cover invalid-address recovery,
duplicate destination rejection, 1/2/4/8 workers, pause/resume/stop/restart,
sustained mainnet-difficulty work, networking disable/enable and loss of peers.

Bootstrap tests use controlled monotonic timestamps and in-process P2P
VERSION/VERACK handshakes. They exercise numeric-IP and hostname classification,
three-peer stability, peer replacement, incomplete readiness, two bootstrap
disconnections, preservation of ordinary peers, zero-peer recovery and disabled
networking. The test connector/DNS resolver are injected; no operational
bootstrap connection is disconnected by these tests.

Hasher validation compares 500 header/nonce combinations individually against
Core's normal hash function, including 0, UINT32_MAX-1 and UINT32_MAX, plus an
aggregate comparison over one million nonces.

Native Windows full unit suite, with the official external script fixture:
**742 passed, 5 skipped, 27,261,742 assertions passed**, no missing-fixture warning.
Fixture SHA256: `cd789a58ec45916e1721cdd14e82ca4c93100959f1cef4e229b22e3bf539f095`.

Final Linux functional tests passed:

- `rpc_generate.py`, seed 701, including final-nonce success and failure.
- `rpc_generate.py --usecli`, seed 702, including the same boundary checks.
- `feature_recycle.py`, seed 703.
- `feature_recycle_spend.py`, seed 704.

The new boundary test initially had fixture/API errors (unset mocktime and
pre-Core-31 hash accessor names). These were corrected, and both RPC/CLI tests
then passed. Final result logs are the per-test `miner-v2-functional-*.log` files.

Native Windows wallet upgrade passed at regtest height 104: old encrypted
wallet opened in the new GUI, old address ownership and balance retained,
backup restored, both original/restored wallets signed and sent funds,
receiver obtained 1.5 coins, full chain validation and GUI restart passed.
Native CLI startnonce argument conversion also passed. Only disposable test
wallets and isolated data were used.

Repository-selected Ruff checks, mypy, include lint and `git diff HEAD --check`
passed for the relevant changes. Remote CI and its entire platform matrix
were not run. The installer was built, not installed over the user's existing
installation. Internet-wide partitions, arbitrary user wallet corruption and
all proxy configurations are outside these tests.

## Measurements on this computer

Sustained isolated mainnet-difficulty mining: 1 worker 10.83 MH/s, 2 workers
21.41 MH/s, 4 workers 40.64 MH/s, 8 workers 70.06 MH/s. Stop completed within
the millisecond timer's zero-ms bucket in these runs; this is not a universal
latency bound. Header-only hashing measured 7.59 MH/s with Core's ordinary
hash function and 11.84 MH/s with midstate reuse. This is a hashing comparison,
not a complete benchmark of the published old miner, nor a reward guarantee.

## Artifacts and cleanup

- `build-win64/bitcoin-win64-setup.exe`
- `build-win64/resatoshi-recycle-upgrade-win64-local.zip`
- `build-win64/SHA256SUMS`

ZIP integrity and its GUI/wrapper bytes were checked against the release
directory. Re-stripping the tested GUI produced identical content except for
the PE timestamp and checksum generated by strip. The exact tested/artifact
hashes are in `build-audit/miner-v2-artifact-sha256.json`.

Temporary nodes, wallets, fixture downloads, helper scripts and temporary
comparison/cache files were removed. Useful binaries, sources, documentation
and validation logs were retained. No real user wallet was copied or changed.
