#!/usr/bin/env python3
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test ASERT difficulty adjustment on the ReSatoshi mainnet.

The precomputed block 1 follows the ReSatoshi genesis after 300 seconds. Verify
that ASERT raises difficulty for block 2 and rejects the unchanged target.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)
from test_framework.blocktools import create_coinbase, nbits_str, target_str

from test_framework.messages import (
    CBlock,
    SEQUENCE_FINAL,
)

COINBASE_SCRIPT_PUBKEY="76a914eadbac7f36c37e39361168b7aaee3cb24a25312d88ac"
INITIAL_N_BITS = 0x1D03A112
INITIAL_TARGET = int("00000003a1120000000000000000000000000000000000000000000000000000", 16)
BLOCK1_TIME = 1788480300
BLOCK1_NONCE = 794256396
BLOCK2_TIME = 1788480600
BLOCK2_NONCE = 2617727490

class MiningMainnetTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.chain = "" # main

    def mine(self, height, prev_hash, node, expected_result=None):
        self.log.debug(f"height={height}")
        block = CBlock()
        block.nVersion = 0x20000000
        block.hashPrevBlock = int(prev_hash, 16)
        block.nTime = BLOCK1_TIME if height == 1 else BLOCK2_TIME
        block.nBits = INITIAL_N_BITS
        block.nNonce = BLOCK1_NONCE if height == 1 else BLOCK2_NONCE
        block.vtx = [create_coinbase(height=height, script_pubkey=bytes.fromhex(COINBASE_SCRIPT_PUBKEY), halving_period=210000)]
        # The alternate mainnet chain was mined with non-timelocked coinbase txs.
        block.vtx[0].nLockTime = 0
        block.vtx[0].vin[0].nSequence = SEQUENCE_FINAL
        block.hashMerkleRoot = block.calc_merkle_root()
        block_hex = block.serialize(with_witness=False).hex()
        self.log.debug(block_hex)
        assert_equal(node.submitblock(block_hex), expected_result)
        if expected_result is not None:
            return prev_hash
        prev_hash = node.getbestblockhash()
        assert_equal(prev_hash, block.hash_hex)
        return prev_hash


    def run_test(self):
        node = self.nodes[0]
        # Clear disk space warning
        node.stderr.seek(0)
        node.stderr.truncate()
        prev_hash = node.getbestblockhash()
        prev_hash = self.mine(1, prev_hash, node)
        assert_equal(node.getblockcount(), 1)

        self.log.info("Check ASERT adjustment with getmininginfo")
        mining_info = node.getmininginfo()
        assert_equal(mining_info['bits'], nbits_str(INITIAL_N_BITS))
        assert_equal(mining_info['target'], target_str(INITIAL_TARGET))

        assert_equal(mining_info['next']['height'], 2)
        assert_equal(mining_info['next']['bits'], nbits_str(0x1D039FF6))

        self.log.info("Reject a second block that retains the legacy target")
        self.mine(2, prev_hash, node, expected_result='bad-diffbits')
        assert_equal(node.getblockcount(), 1)


if __name__ == '__main__':
    MiningMainnetTest(__file__).main()
