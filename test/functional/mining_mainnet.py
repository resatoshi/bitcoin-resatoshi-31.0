#!/usr/bin/env python3
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test ASERT difficulty adjustment on an alternate mainnet.

The first precomputed difficulty-1 block follows genesis. Its 126-second solve
time makes ASERT increase difficulty for block 2. Verify the new target and
that a block retaining the legacy target is rejected.

It uses an alternate mainnet chain. See data/README.md for how it was generated.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)
from test_framework.blocktools import (
    DIFF_1_N_BITS,
    DIFF_1_TARGET,
    create_coinbase,
    nbits_str,
    target_str
)

from test_framework.messages import (
    CBlock,
    SEQUENCE_FINAL,
)

import json
import os

# See data/README.md
COINBASE_SCRIPT_PUBKEY="76a914eadbac7f36c37e39361168b7aaee3cb24a25312d88ac"

class MiningMainnetTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.chain = "" # main

    def add_options(self, parser):
        parser.add_argument(
            '--datafile',
            default='data/mainnet_alt.json',
            help='Block data file (default: %(default)s)',
        )

    def mine(self, height, prev_hash, blocks, node, expected_result=None):
        self.log.debug(f"height={height}")
        block = CBlock()
        block.nVersion = 0x20000000
        block.hashPrevBlock = int(prev_hash, 16)
        block.nTime = blocks['timestamps'][height - 1]
        block.nBits = DIFF_1_N_BITS if height < 2016 else DIFF_4_N_BITS
        block.nNonce = blocks['nonces'][height - 1]
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
        self.log.info("Load alternative mainnet blocks")
        path = os.path.join(os.path.dirname(os.path.realpath(__file__)), self.options.datafile)
        prev_hash = node.getbestblockhash()
        blocks = None
        with open(path) as f:
            blocks = json.load(f)
            n_blocks = len(blocks['timestamps'])
            assert_equal(n_blocks, 2016)

        prev_hash = self.mine(1, prev_hash, blocks, node)
        assert_equal(node.getblockcount(), 1)

        self.log.info("Check ASERT adjustment with getmininginfo")
        mining_info = node.getmininginfo()
        assert_equal(mining_info['difficulty'], 1)
        assert_equal(mining_info['bits'], nbits_str(DIFF_1_N_BITS))
        assert_equal(mining_info['target'], target_str(DIFF_1_TARGET))

        assert_equal(mining_info['next']['height'], 2)
        assert_equal(mining_info['next']['bits'], nbits_str(0x1d00ff83))

        self.log.info("Reject a second block that retains the legacy target")
        self.mine(2, prev_hash, blocks, node, expected_result='bad-diffbits')
        assert_equal(node.getblockcount(), 1)


if __name__ == '__main__':
    MiningMainnetTest(__file__).main()
