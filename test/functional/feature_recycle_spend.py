#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check expiry-block spends, fees, mempool eviction and rollback together."""

from decimal import Decimal
import time

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class RecycleSpendTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle", "-coinstatsindex", "-checklevel=4"] for _ in range(2)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def block(self, coinbase, txs=None):
        node = self.nodes[0]
        tip = node.getbestblockhash()
        block = create_block(int(tip, 16), coinbase, max(int(time.time()), node.getblockheader(tip)["time"] + 1), txlist=txs)
        if txs:
            add_witness_commitment(block)
        block.solve()
        return block

    def check_stats(self):
        for node in self.nodes:
            self.wait_until(lambda: node.getindexinfo()["coinstatsindex"]["synced"])
            indexed = node.gettxoutsetinfo("muhash")
            scanned = node.gettxoutsetinfo("muhash", use_index=False)
            for field in ("height", "bestblock", "txouts", "total_amount", "muhash"):
                assert_equal(indexed[field], scanned[field])

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("expiry-spend")
        wallet = node.get_wallet_rpc("expiry-spend")
        address = wallet.getnewaddress()
        script = bytes.fromhex(wallet.getaddressinfo(address)["scriptPubKey"])
        origin = create_coinbase(1, script_pubkey=script)
        origin.vout = [CTxOut(2 * COIN, script), CTxOut(2 * COIN, script)]
        assert_equal(node.submitblock(self.block(origin).serialize().hex()), None)
        self.sync_all()
        self.generatetodescriptor(node, 199, "raw(6a)")
        before = node.gettxoutsetinfo()["hash_serialized_3"]
        signed = []
        txids = []
        for vout in range(2):
            raw = wallet.createrawtransaction([{"txid": origin.txid_hex, "vout": vout}], {address: Decimal("1.999")})
            result = wallet.signrawtransactionwithwallet(raw)
            assert result["complete"]
            signed.append(result["hex"])
            txids.append(node.sendrawtransaction(result["hex"]))
        self.sync_all()
        # Both candidate spends prevent recycling; only their fees are added.
        assert_equal(node.getblocktemplate({"rules": ["segwit"]})["coinbasevalue"], 25 * COIN + 200_000)

        coinbase = create_coinbase(201, script_pubkey=script)
        coinbase.vout[0].nValue += COIN + 100_000 + 1
        excessive = self.block(coinbase, signed[:1])
        assert_equal(node.submitblock(excessive.serialize().hex()), "bad-cb-amount")
        assert_equal(node.gettxoutsetinfo()["hash_serialized_3"], before)
        coinbase.vout[0].nValue -= 1
        expiry = self.block(coinbase, signed[:1])
        assert_equal(node.submitblock(expiry.serialize().hex()), None)
        self.sync_all()
        for peer in self.nodes:
            assert_equal(peer.getrawmempool(), [])
            assert_equal(peer.gettxout(origin.txid_hex, 0), None)
            assert_equal(peer.gettxout(origin.txid_hex, 1), None)
            assert_equal(peer.gettxout(txids[0], 0)["value"], Decimal("1.999"))
            assert peer.verifychain(4, 0)
        assert_equal(wallet.getbalance(), Decimal("1.999"))
        assert_equal(wallet.listunspent()[0]["expiry_height"], 401)
        self.check_stats()

        for peer in self.nodes:
            peer.invalidateblock(expiry.hash_hex)
            assert_equal(peer.gettxoutsetinfo()["hash_serialized_3"], before)
            for vout in range(2):
                assert_equal(peer.gettxout(origin.txid_hex, vout, False)["value"], 2)
        self.check_stats()
        self.generatetoaddress(node, 3, address)
        self.check_stats()
        self.restart_node(1)
        self.connect_nodes(0, 1)
        self.sync_all()
        self.check_stats()


if __name__ == "__main__":
    RecycleSpendTest(__file__).main()
