// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <node/recycle.h>
#include <primitives/block.h>
#include <script/script.h>
#include <undo.h>

#include <boost/test/unit_test.hpp>

namespace {

CBlock CoinbaseBlock(CAmount value, int unique)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout.SetNull();
    tx.vin[0].scriptSig = CScript{} << unique;
    tx.vout.emplace_back(value, CScript{} << OP_TRUE);
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(std::move(tx)));
    return block;
}

} // namespace

BOOST_AUTO_TEST_SUITE(recycle_tests)

BOOST_AUTO_TEST_CASE(expiry_reward_and_disconnect)
{
    CCoinsView base;
    CCoinsViewCache view{&base};
    constexpr CAmount BASE_REWARD{50 * COIN};
    constexpr CAmount EXPIRED_VALUE{COIN / 2};

    CBlock origin{CoinbaseBlock(EXPIRED_VALUE, 1)};
    AddCoins(view, *origin.vtx[0], /*height=*/1);
    CTxUndo early_undo;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, EXPIRED_VALUE, early_undo, error));
    BOOST_CHECK(early_undo.vprevout.empty());

    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock expiry{CoinbaseBlock(BASE_REWARD + EXPIRED_VALUE, 2)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height), EXPIRED_VALUE);
    AddCoins(view, *expiry.vtx[0], expiry_height);

    CTxUndo recycle_undo;
    std::vector<COutPoint> expired;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD,
                                               recycle_undo, error, &expired));
    BOOST_CHECK_EQUAL(expired.size(), 1U);
    BOOST_CHECK(!view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 0);

    BOOST_REQUIRE(node::recycle::DisconnectBlock(view, expiry_height, &recycle_undo));
    BOOST_CHECK(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 0);
}

BOOST_AUTO_TEST_CASE(unclaimed_value_stays_in_pool_and_reward_is_capped)
{
    CCoinsView base;
    CCoinsViewCache view{&base};
    constexpr CAmount BASE_REWARD{50 * COIN};

    CBlock origin{CoinbaseBlock(2 * COIN, 3)};
    AddCoins(view, *origin.vtx[0], /*height=*/1);
    CTxUndo unused;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, 2 * COIN, unused, error));

    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock expiry{CoinbaseBlock(BASE_REWARD, 4)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height), COIN);
    AddCoins(view, *expiry.vtx[0], expiry_height);
    CTxUndo recycle_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD,
                                               recycle_undo, error));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 2 * COIN);
}

BOOST_AUTO_TEST_SUITE_END()
