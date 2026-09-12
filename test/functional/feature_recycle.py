#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Exercise real disk, wallet, index, and reorg paths at UTXO expiry."""

from decimal import Decimal
import http.client
import subprocess
import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import COIN, CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class RecycleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle", "-coinstatsindex", "-checklevel=4"] for _ in range(2)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def check_stats(self):
        self.sync_all()
        for node in self.nodes:
            self.wait_until(lambda: node.getindexinfo()["coinstatsindex"]["synced"])
            indexed = node.gettxoutsetinfo("muhash")
            scanned = node.gettxoutsetinfo("muhash", use_index=False)
            for field in ("height", "bestblock", "txouts", "total_amount", "muhash"):
                assert_equal(indexed[field], scanned[field])

    def submit(self, coinbase):
        node = self.nodes[0]
        tip = node.getbestblockhash()
        block = create_block(int(tip, 16), coinbase, max(int(time.time()), node.getblockheader(tip)["time"] + 1))
        block.solve()
        assert_equal(node.submitblock(block.serialize().hex()), None)
        self.sync_all()
        return block.hash_hex

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("expiry")
        wallet = node.get_wallet_rpc("expiry")
        address = wallet.getnewaddress()
        script = bytes.fromhex(wallet.getaddressinfo(address)["scriptPubKey"])
        origin = create_coinbase(1, script_pubkey=script)
        origin.vout = [CTxOut(COIN // 10, script) for _ in range(278)]
        self.submit(origin)
        origin_txid = origin.txid_hex

        # Only the first block has spendable outputs, making pool accounting
        # deterministic. Its schedule is larger than MAX_SCRIPT_SIZE.
        self.generatetodescriptor(node, 199, "raw(6a)")
        assert_equal(wallet.getbalance(), Decimal("27.8"))
        assert_equal(len(wallet.listunspent()), 278)
        assert_equal(wallet.listunspent()[0]["expiry_height"], 201)
        assert_equal(wallet.listunspent()[0]["blocks_until_expiry"], 1)
        self.check_stats()
        before = node.gettxoutsetinfo()["hash_serialized_3"]

        self.restart_node(1, extra_args=self.extra_args[1] + ["-dbbatchsize=1", "-dbcrashratio=1"])
        self.connect_nodes(0, 1)
        expiry_hash = self.generatetoaddress(node, 1, address)[0]
        self.log.info("Recover a partial chainstate flush of the large expiry block")
        with self.nodes[1].assert_debug_log(["Simulating a crash. Goodbye."]):
            try:
                self.nodes[1].gettxoutsetinfo("muhash", use_index=False)
            except (http.client.RemoteDisconnected, ConnectionResetError):
                pass
            except subprocess.CalledProcessError as error:
                # --usecli reports the deliberate RPC disconnect as a failed
                # subprocess. Do not accept unrelated CLI errors. The crash log
                # and stopped process are still required below in either mode.
                assert self.nodes[1].use_cli
                assert_equal(error.returncode, 1)
                assert "Could not connect to the server" in error.output
            self.nodes[1].wait_until_stopped(timeout=20)
        self.start_node(1)
        self.connect_nodes(0, 1)
        self.check_stats()
        assert_equal(node.gettxout(origin_txid, 0), None)
        assert_equal(wallet.getbalance(), 0)
        assert_equal(wallet.listunspent(), [])
        expiry = node.getblock(expiry_hash, 2)
        assert_equal(expiry["tx"][0]["vout"][0]["value"], 26)  # 25 subsidy + 1 recycled
        raw = wallet.createrawtransaction([{"txid": origin_txid, "vout": 0}], {address: Decimal("0.01")})
        assert_raises_rpc_error(-4, "expired", wallet.fundrawtransaction, raw)

        # Restart forces block verification to consume serialized undo data.
        self.restart_node(0)
        self.connect_nodes(0, 1)
        node.loadwallet("expiry")
        wallet = node.get_wallet_rpc("expiry")
        assert_equal(wallet.getbalance(), 0)
        self.check_stats()
        for peer in self.nodes:
            peer.invalidateblock(expiry_hash)
        self.check_stats()
        assert_equal(node.gettxoutsetinfo()["hash_serialized_3"], before)
        assert_equal(wallet.getbalance(), Decimal("27.8"))
        assert_equal(len(wallet.listunspent()), 278)

        # A miner may claim less than the allowed recycle reward; unclaimed
        # value remains in the pool. Reorg this alternative through disk too.
        partial = create_coinbase(201, script_pubkey=script)
        partial.vout[0].nValue += COIN // 2
        self.submit(partial)
        self.check_stats()
        self.generatetodescriptor(node, 27, "raw(6a)")
        self.check_stats()
        last = self.generatetoaddress(node, 1, address)[0]
        # 27.8 - 0.5 - 27 = 0.3 left in the pool.
        assert_equal(node.getblock(last, 2)["tx"][0]["vout"][0]["value"], Decimal("25.3"))
        empty = self.generatetoaddress(node, 1, address)[0]
        assert_equal(node.getblock(empty, 2)["tx"][0]["vout"][0]["value"], 25)
        self.check_stats()
        self.restart_node(1)
        self.connect_nodes(0, 1)
        self.check_stats()


if __name__ == "__main__":
    RecycleTest(__file__).main()
