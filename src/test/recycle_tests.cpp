// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <node/recycle.h>
#include <primitives/block.h>
#include <script/script.h>
#include <undo.h>

#include <algorithm>

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
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo early_undo;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, EXPIRED_VALUE, early_undo, error));
    BOOST_CHECK(!view.HaveCoin(COutPoint{Txid{}, 1}));
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
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
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

BOOST_AUTO_TEST_CASE(reward_boundaries_and_excess_claim)
{
    constexpr CAmount BASE_REWARD{50 * COIN};
    constexpr CAmount REMAINDER{COIN / 3};

    CCoinsView base;
    CCoinsViewCache view{&base};
    CBlock origin{CoinbaseBlock(REMAINDER, 10)};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo unused;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, REMAINDER, unused, error));

    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock exact{CoinbaseBlock(BASE_REWARD + REMAINDER, 11)};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, exact, expiry_height), REMAINDER);

    CBlock excessive{CoinbaseBlock(BASE_REWARD + REMAINDER + 1, 12)};
    AddCoins(view, *excessive.vtx[0], expiry_height);
    CTxUndo undo;
    BOOST_CHECK(!node::recycle::ConnectBlock(view, excessive, expiry_height, BASE_REWARD, undo, error));
    BOOST_CHECK_EQUAL(error, "coinbase claims excessive recycle reward");
}

BOOST_AUTO_TEST_CASE(pool_accounting_stress)
{
    constexpr CAmount BASE_REWARD{50 * COIN};
    CCoinsView base;
    CCoinsViewCache view{&base};
    CAmount expected_pool{0};

    for (int iteration = 0; iteration < 1'000; ++iteration) {
        const int origin_height{iteration + 1};
        const CAmount expiring{(1 + iteration % 4) * (COIN / 2)};
        CBlock origin{CoinbaseBlock(expiring, 1'000 + iteration)};
        AddCoins(view, *origin.vtx[0], origin_height);
        CTxUndo unused;
        std::string error;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, origin_height, expiring, unused, error));

        const int expiry_height{origin_height + node::recycle::EXPIRY_BLOCKS};
        const CAmount available{expected_pool + expiring};
        const CAmount claimed{std::min(node::recycle::MAX_REWARD, available)};
        CBlock expiry{CoinbaseBlock(BASE_REWARD + claimed, 10'000 + iteration)};
        BOOST_REQUIRE_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height), claimed);
        AddCoins(view, *expiry.vtx[0], expiry_height);
        CTxUndo undo;
        BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD, undo, error));
        expected_pool = available - claimed;
        BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), expected_pool);
        BOOST_REQUIRE(expected_pool >= 0);
    }
}

BOOST_AUTO_TEST_CASE(partial_reward_fee_separation_and_reorg)
{
    constexpr CAmount SUBSIDY{50 * COIN};
    constexpr CAmount FEES{12'345};
    constexpr CAmount EXPIRED{2 * COIN};
    constexpr CAmount PARTIAL_REWARD{COIN / 3};

    CCoinsView base;
    CCoinsViewCache view{&base};
    CBlock origin{CoinbaseBlock(EXPIRED, 20'000)};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo unused;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, EXPIRED, unused, error));

    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock expiry{CoinbaseBlock(SUBSIDY + FEES + PARTIAL_REWARD, 20'001)};
    AddCoins(view, *expiry.vtx[0], expiry_height);
    CTxUndo expiry_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, SUBSIDY + FEES,
                                               expiry_undo, error));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), EXPIRED - PARTIAL_REWARD);

    // Crash recovery may replay a transition whose UTXO and recycle changes
    // were already written. Reapplying it must be idempotent.
    BOOST_REQUIRE(node::recycle::RollforwardBlock(view, expiry, expiry_height,
                                                   SUBSIDY + FEES, &expiry_undo));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), EXPIRED - PARTIAL_REWARD);

    // Reorg the partially rewarded expiry block out and replace it with a block
    // that claims no recycle reward. The expiry must be applied exactly once.
    BOOST_REQUIRE(node::recycle::DisconnectBlock(view, expiry_height, &expiry_undo));
    BOOST_REQUIRE_EQUAL(node::recycle::PoolBalance(view), 0);
    BOOST_REQUIRE(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), 0}));
    CBlock alternate{CoinbaseBlock(SUBSIDY + FEES, 20'002)};
    AddCoins(view, *alternate.vtx[0], expiry_height);
    CTxUndo alternate_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, alternate, expiry_height, SUBSIDY + FEES,
                                               alternate_undo, error));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), EXPIRED);
}

BOOST_AUTO_TEST_CASE(spent_in_expiry_block_is_not_recycled)
{
    constexpr CAmount VALUE{3 * COIN / 4};
    constexpr CAmount BASE_REWARD{50 * COIN};

    CCoinsView base;
    CCoinsViewCache view{&base};
    CBlock origin{CoinbaseBlock(VALUE, 30'000)};
    const COutPoint origin_out{origin.vtx[0]->GetHash(), 0};
    AddCoins(view, *origin.vtx[0], /*nHeight=*/1);
    CTxUndo unused;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, /*height=*/1, VALUE, unused, error));

    CMutableTransaction spend;
    spend.vin.emplace_back(origin_out);
    spend.vout.emplace_back(VALUE, CScript{} << OP_TRUE);
    const int expiry_height{1 + node::recycle::EXPIRY_BLOCKS};
    CBlock expiry{CoinbaseBlock(BASE_REWARD, 30'001)};
    expiry.vtx.push_back(MakeTransactionRef(std::move(spend)));

    // Template accounting must exclude an output spent by the candidate block.
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, expiry, expiry_height), 0);

    // ConnectBlock calls recycle accounting after normal transaction updates.
    Coin spent;
    BOOST_REQUIRE(view.SpendCoin(origin_out, &spent));
    AddCoins(view, *expiry.vtx[0], expiry_height);
    AddCoins(view, *expiry.vtx[1], expiry_height);
    CTxUndo recycle_undo;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, expiry_height, BASE_REWARD,
                                               recycle_undo, error));
    BOOST_CHECK_EQUAL(node::recycle::PoolBalance(view), 0);

    // The replacement output is a normal new UTXO. Its lifetime starts at
    // this confirmation height, not at the spent output's origin height.
    const COutPoint renewed_out{expiry.vtx[1]->GetHash(), 0};
    BOOST_REQUIRE(view.HaveCoin(renewed_out));
    CBlock before_renewed_expiry{CoinbaseBlock(BASE_REWARD, 30'002)};
    const int before_height{expiry_height + node::recycle::EXPIRY_BLOCKS - 1};
    BOOST_CHECK_EQUAL(node::recycle::AvailableReward(view, before_renewed_expiry, before_height), 0);

    CBlock renewed_expiry{CoinbaseBlock(BASE_REWARD, 30'003)};
    const int renewed_expiry_height{expiry_height + node::recycle::EXPIRY_BLOCKS};
    AddCoins(view, *renewed_expiry.vtx[0], renewed_expiry_height);
    CTxUndo renewed_undo;
    std::vector<COutPoint> renewed_expired;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, renewed_expiry, renewed_expiry_height,
                                               BASE_REWARD, renewed_undo, error, &renewed_expired));
    BOOST_CHECK(std::find(renewed_expired.begin(), renewed_expired.end(), renewed_out) != renewed_expired.end());
    BOOST_CHECK(!view.HaveCoin(renewed_out));
    BOOST_CHECK_GE(node::recycle::PoolBalance(view), VALUE);
}

BOOST_AUTO_TEST_SUITE_END()
