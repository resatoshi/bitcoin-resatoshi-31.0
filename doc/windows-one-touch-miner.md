# ReSatoshi one-touch miner for Windows

The Windows installer contains a full validating ReSatoshi node, a local
descriptor wallet, and a CPU miner. Start **ReSatoshi** from the Start menu.
The node and wallet start together and shut down cleanly when the window is
closed.

On a new installation, the application creates the `resatoshi-miner` wallet
only if no wallet already exists. Its keys remain in the ReSatoshi data
directory on that computer. The application does not send wallet keys or use
the bootstrap nodes' RPC service.

## First use

1. Wait until the dashboard says **Synchronized**. The mining button remains
   unavailable during initial block download.
2. Select 1, 2, 4, or 8 CPU threads and press **Start Mining**. Mining never
   starts automatically. Once started, mining pauses when peers are lost,
   networking is disabled, or synchronization falls behind, and resumes when
   ready again. Press Stop Mining to cancel this automatic resumption.
   Every reward address belongs to the displayed local
   wallet.
3. Press **Encrypt Wallet**, choose a strong unique passphrase, and store it
   offline. A forgotten passphrase cannot be recovered.
4. Press **Back Up Wallet** and save the backup on offline media. Repeat the
   backup after important wallet changes. **Restore Backup** opens the standard
   local restore flow and never uploads the file.

The send panel validates the destination and amount, estimates or accepts a
chosen fee, and presents destination, amount, fee, and total for final review
before signing and broadcasting.

## Renewing UTXOs

The **Renew UTXOs** panel lists only confirmed, mature, unlocked, safe outputs
that this wallet can spend and that have not expired. It shows their creation
and expiry heights, remaining blocks, and an estimate based on ten-minute
blocks. **Select Near Expiry** means approximately one year or less remains.

Renewal is an ordinary self-transfer, not an edit to expiry metadata. First
select outputs and press **Renew UTXOs — Preview**. The application shows the
estimated fee, net result, and fresh receiving address without broadcasting.
Continuing requests the wallet passphrase when needed, constructs the exact
transaction, shows its exact fee in a final confirmation, and only then sends
it. The resulting output begins a new lifetime at its confirmation height.
Normal Bitcoin Core fee estimation is preferred. Before enough history exists,
the renewal uses 1 sat/vB, raised when necessary to the node's current mempool
or required minimum. The final dialog reports actual transaction vsize, total
fee, and sat/vB; an unusually high fee requires a separate warning acceptance.
Large selections are split into transactions of at most 100 inputs, each with
its own fresh wallet address, instead of merging the whole wallet into one
output. Keep the displayed TXIDs until they are confirmed.

## Peer discovery

An empty node initially asks only these ReSatoshi DNS bootstrap names:

* `resatoshi-seed.freeddns.org:19333`
* `resatoshi-seed.duckdns.org:19333`

After that, normal P2P address relay and the local peer database are used. DNS
bootstrap is used again when the node cannot find enough usable peers. No
numeric public bootstrap address is compiled into the ReSatoshi mainnet
configuration.

Uninstalling the program does not need to delete the wallet or chain data.
Back up the wallet before deliberately removing the data directory.


## CPU mining implementation (local 31.0.4 build)

Workers use the local Core mining interface, reuse a block template for up to
one second, and assign a unique coinbase extranonce to every template. Work is
cancelled on Stop Mining and reconsidered at most every 4,096 hashes when the
active chain tip or network readiness changes. Stopping from the dashboard is
asynchronous. Node shutdown still waits for owned worker threads to finish.

The hashrate display uses actual hash attempts and elapsed monotonic time;
it includes attempts in successful batches. A SHA256 midstate avoids repeating
the first compression block of an unchanged header. Hash output is checked
against the original Core hash function in tests, including nonce boundaries.
Mining does not change subsidy, recycle eligibility, BTC/sat units or addresses.

An old tip alone no longer prevents local CPU mining. The readiness checks
for this case are described below; peer presence alone is still insufficient
above genesis. Normal block download and validation continue unchanged.


## Bootstrap handoff

The GUI manages bootstrap connections once per node, independently of the
number of open wallets. After initial synchronization finishes, three
non-bootstrap, handshake-complete peers advertising block-serving services
must each remain eligible for 60 seconds before bootstraps are disconnected.
Discovery-only, feeler and private-broadcast connections do not count. Each
replacement connection starts its own interval. A temporarily unavailable
statistics snapshot skips that observation rather than resetting the interval.
Remaining peers are checked again before each disconnect. Nodes are not banned.

The manager registers bootstrap names with Core's existing added-node list.
Core's background connection thread owns DNS, sockets and retries; the GUI
creates no DNS/connect worker or future. The names and default port come from
chain parameters. The manager unregisters only entries it added, preserving
user-configured addnodes. Explicit `-connect` configurations disable this
automatic policy, and disabling networking unregisters its pending entries.

When the peer count reaches zero, the manager immediately requests bootstrap
registration again. Actual connection scheduling belongs to Core: its added-node
loop normally polls every two seconds and waits 60 seconds after attempts.
Until handoff, Core continues retrying registered seeds even with one or two
ordinary peers. After handoff, one or two peers do not register seeds again.
Core's normal discovery may reconnect a bootstrap, which is retired again
while the replacement conditions still hold.

Identity uses hostnames and cached base-seed resolutions from Core's connection
thread, including numeric-IP peers already connected when a seed is resolved.
Service-filtered DNS peer lists are not classified as bootstrap servers.
Unresolved seed identity delays handoff. Name proxies are respected without
extra local DNS queries. This GUI policy runs only on mainnet. Removing the
GUI's extra workers does not impose a timeout on Core's own OS DNS resolver or
guarantee a maximum duration for whole-node shutdown.

## Mining after a long network pause

The local CPU miner can mine on an old mainnet tip without changing
`-maxtipage`. The mining button and running workers use the same readiness
check. Loading/reindexing, missing blocks behind the best known header, and
insufficient chain work still prevent mining. Networking and a completed
relay connection are required.

When Core still reports initial block download because the tip is old, a
block-serving peer must have announced a header at the local tip height.
A handshake alone does not suffice above genesis. Connected peers advertising
a higher starting/header height, headers presynchronization, or block downloads
in flight keep the miner paused. Core's existing initial header request starts
from the preceding block, so an up-to-date peer also announces an old tip when
no new blocks exist. The existing genesis bootstrap exception is retained.

This changes only the local GUI/CPU mining decision on mainnet. Core's global
IBD flag, transaction relay and mining RPC policy remain unchanged, as do
proof of work, difficulty adjustment, rewards, addresses and historical blocks.
The bootstrap manager still retains bootstrap connections while Core reports
IBD. A newly mined current-time block can let Core finish IBD normally.

Readiness reflects the information available from connected peers, not proof
that an unreachable peer has no stronger chain. Mining is not automatically
started; press Start Mining as usual. A running miner pauses when prerequisites
are lost and resumes when they return.
