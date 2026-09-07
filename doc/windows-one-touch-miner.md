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
   starts automatically. Every reward address belongs to the displayed local
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
