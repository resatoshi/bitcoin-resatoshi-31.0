# Local Windows miner 31.0.4 validation

The GUI CPU miner no longer requires an operator to increase `-maxtipage`
merely because the mainnet tip is old. This is a local mining-readiness change,
not a change to global IBD, difficulty, proof of work, rewards, recycle rules,
historical blocks, wallet units or addresses. The main-fix branch is unchanged.

## Readiness

The mining button and CPU workers call the same node interface. Networking,
a completed relay connection, loaded chainstate, minimum chain work, and a
fully processed best-known header chain are required. When Core reports IBD
on mainnet above genesis, a block-serving peer must have announced a header
at the local tip height. Handshake completion alone does not qualify. Higher
advertised/header heights, headers presynchronization or blocks in flight
prevent the old-tip exception. The pre-existing genesis exception remains.

This reuses Core's existing peer statistics. Initial getheaders starts from
the preceding block, so a current peer announces the final header even when
no new blocks have been mined. No extra timer, DNS query or wire message is
introduced. Information from peers cannot establish the absence of a stronger
chain on unreachable peers.

Global Core IBD, transaction relay and RPC-based mining policies remain
unchanged. Bootstrap handoff still waits for Core IBD completion. After a
current-time block is mined and accepted, normal IBD completion can occur.

## Validation scope

The mainnet GUI regression fixture uses public block 1 and a clock five days
after its timestamp. It covers a received header with a missing block body,
a locally validated block without a peer header announcement, and mining after
both are present while Core still reports IBD. Network disable/enable must
pause/resume the miner. Tests use fake local peers and disposable wallets;
no operational node or wallet is touched.

Native Windows Qt suites passed 27 results (including setup/cleanup), with
zero failures and process exit code 0. Both old-tip orderings passed using
actual serialized P2P headers, and existing miner, bootstrap and wallet GUI
tests passed. Linux builds completed; `feature_recycle.py`,
`feature_recycle_spend.py` and `rpc_generate.py` passed on 31.0.4. Include lint
and whitespace checks passed.

Final results and exact artifact/source hashes are recorded in the ignored
`build-audit/` directory and `build-win64/SHA256SUMS` plus
`build-win64/SOURCE-COMMITS.txt`. Historical full-suite and wallet migration
results remain in earlier reports. Remote CI and deployment are not part of
this local change.
