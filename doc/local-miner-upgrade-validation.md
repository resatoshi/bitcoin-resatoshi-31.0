# Local node and Windows miner upgrade validation

Historical 31.0.1 results. For the replacement miner, see
[local-miner-v2-validation.md](local-miner-v2-validation.md).

Date: 2026-09-11. Integration branch: `fix/windows-miner-recycle-upgrade`.
Base: `25b9e79273` (existing Windows miner and nonce fix), with the local
`fix/recycle-state-integrity` changes applied. No commits were pushed and no
production installation was replaced.

## Existing operational chainstate

Only `blocks` and `chainstate` were copied from the operating bootstrap node.
File sizes and modification times were unchanged during copying. No wallet,
authentication file or operational configuration was copied. An online file
copy is not a general substitute for a coordinated backup: this particular
copy was subsequently opened and fully validated by the old binary before
being used as the upgrade fixture.

The old isolated node reached height 1,255, matching the operating chain hash:
`00000000850a09051dce9c95df4714a78e6373762f941bad62135f206e426a03`.
After clean shutdown, a copy of that validated database was opened by the
recycle-fix Linux node. Both versions passed `verifychain 4 0` and agreed on
1,255 UTXOs, 62,750 coins, and serialized UTXO hash
`3dd76691d67193d7efc5a8e91e46dd3818206597fb8adbec990ee2b7fbfa3b19`.
Restarting the new node retained the same state. Reopening the upgraded
database with the old binary was rejected. The production node remained
running throughout. See `build-audit/chainstate-upgrade.json`.

## Native Windows wallet upgrade

The integrated miner was cross-compiled with GCC 13-posix and Qt 6.8.3, then
executed on native Windows 11 (10.0.26200.9445), not Wine.

In an isolated regtest data directory, the old Windows daemon created a new
descriptor wallet, mined funds, encrypted the wallet and wrote a backup. After
clean shutdown, the new `bitcoin-qt.exe` opened the same directory and wallet.
The chain tip, original address ownership and spendable balance were retained.
The new GUI's RPC server unlocked the wallet, sent a transaction and mined its
confirmation. The old backup was also restored under another wallet name;
rescanning found the same balance, and the restored wallet successfully
unlocked, signed and sent a second transaction. Full validation and another
GUI restart passed at regtest height 103. No real user wallet or mainnet funds
were used. See `build-audit/windows-wallet-upgrade.json`.

Regtest used `-test=recycle` on the new program to exercise its recycle-aware
startup. This is a test-only 200-block lifetime; mainnet expiry remains
5,256,000 blocks. Production addresses and BTC/sat units are retained.

## Findings resolved during integration

- The miner branch's renewal unit test skipped a height whose schedule is now
  required by strict state checking. The sparse fixture now creates that
  schedule instead of relying on a missing record being treated as zero.
- `CpuMiner::worker` caught `std::exception`, while `executeRpc` can throw
  JSON-RPC errors as `UniValue`. An RPC error could escape the worker and
  terminate the GUI. The worker now retains the RPC error message and stops
  mining without terminating the application.
- Added a GUI test that deliberately submits an invalid address, verifies
  controlled failure, then mines with one and two workers, stops them, checks
  that the height stops changing, and restarts successfully. Its fixture
  explicitly initializes the mining interface and RPC readiness.

## Windows checks and artifacts

The final native GUI suite passed, including the CPU worker error/restart test
and existing wallet, send, renewal-preview, URI, RPC console and address-book
checks. The Windows full unit run passed 741 cases, with five skipped cases and
one warning-only case because the external script fixture was absent; all
27,043,984 assertions passed. The external fixture then passed separately with
141,917 assertions and no warnings; its result is recorded in
`build-audit/windows-script-assets.log`.

Build and installer generation succeeded. Artifacts are:

- `build-win64/bitcoin-win64-setup.exe`
- `build-win64/resatoshi-recycle-upgrade-win64-local.zip`
- `build-win64/SHA256SUMS`

The installer was built but not installed over the user's existing program.
The portable GUI binary was executed in the isolated Windows tests. The local
build is not a published or signed release. Remote GitHub CI, arbitrary legacy
database corruption, pruning-related recovery and other users' individual
wallets are outside these results. Preserve a wallet backup and its passphrase;
an old executable alone is not a supported rollback for an upgraded chainstate.

Temporary test nodes, wallets, source-data copies, downloaded fixtures and
helper copies are removed after validation. Build outputs and result logs are
retained for review.
