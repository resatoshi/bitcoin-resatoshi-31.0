#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Exercise the wallet primitives used by the Renew UTXOs GUI on regtest."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class RenewUtxosTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="renew-test")
        wallet = node.get_wallet_rpc("renew-test")
        self.generatetoaddress(node, 101, wallet.getnewaddress())

        old = wallet.listunspent(minconf=1)[0]
        old_outpoint = {"txid": old["txid"], "vout": old["vout"]}
        destination = wallet.getnewaddress(label="Renewed UTXO", address_type="bech32")
        balance_before = wallet.getbalances()["mine"]["trusted"]

        result = wallet.send(
            outputs=[{destination: old["amount"]}],
            options={
                "inputs": [old_outpoint],
                "add_inputs": False,
                "subtract_fee_from_outputs": [0],
            },
        )
        txid = result["txid"]
        decoded = wallet.gettransaction(txid=txid, verbose=True)["decoded"]
        assert_equal(len(decoded["vin"]), 1)
        assert_equal(decoded["vin"][0]["txid"], old["txid"])
        assert_equal(decoded["vin"][0]["vout"], old["vout"])
        assert_equal(node.gettxout(old["txid"], old["vout"]), None)

        renewed = next(output for output in decoded["vout"] if destination in output["scriptPubKey"].get("address", ""))
        fee = -wallet.gettransaction(txid)["fee"]
        assert_equal(renewed["value"], old["amount"] - fee)
        assert_equal(wallet.getbalances()["mine"]["trusted"], balance_before - fee)

        confirmation_block = self.generatetoaddress(node, 1, wallet.getnewaddress())[0]
        confirmed = wallet.gettransaction(txid)
        assert_equal(confirmed["confirmations"], 1)
        assert_equal(confirmed["blockhash"], confirmation_block)

        self.restart_node(0)
        node.loadwallet("renew-test")
        wallet = node.get_wallet_rpc("renew-test")
        assert_equal(wallet.gettransaction(txid)["confirmations"], 1)
        assert any(u["txid"] == txid and u["address"] == destination for u in wallet.listunspent(minconf=1))

        node.invalidateblock(confirmation_block)
        node.syncwithvalidationinterfacequeue()
        assert_equal(wallet.gettransaction(txid)["confirmations"], 0)
        assert txid in node.getrawmempool()
        self.generatetoaddress(node, 1, wallet.getnewaddress())
        assert_equal(wallet.gettransaction(txid)["confirmations"], 1)


if __name__ == "__main__":
    RenewUtxosTest(__file__).main()
