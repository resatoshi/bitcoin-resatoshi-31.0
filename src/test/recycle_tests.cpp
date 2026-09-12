// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <node/recycle.h>
#include <node/recycle_serialization.h>
#include <test/util/setup_common.h>
#include <txdb.h>
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
}


BOOST_FIXTURE_TEST_CASE(large_state_disk_and_undo_roundtrip, BasicTestingSetup)
{
    for (int count : {200, 277, 278, 1000}) {
        const auto path{m_path_root / fs::PathFromString(strprintf("coins-%d", count))};
        CBlock origin{CoinbaseBlock(count, count)};
        CMutableTransaction tx{*origin.vtx[0]};
        tx.vout.assign(count, CTxOut{1, CScript{} << OP_TRUE});
        origin.vtx[0] = MakeTransactionRef(std::move(tx));
        const COutPoint schedule_key{Txid{}, 1'000'001};
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {}};
            CCoinsViewCache view{&db};
            AddCoins(view, *origin.vtx[0], 1);
            CTxUndo unused;
            std::string error;
            BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, 1, count, unused, error));
            view.SetBestBlock(uint256{1});
            view.Flush();
        }
        {
            CCoinsViewDB db{{.path = path, .cache_bytes = 1 << 20}, {}};
            CCoinsViewCache view{&db};
            BOOST_REQUIRE(node::recycle::ScheduleValid(view, 1));
            const auto original_schedule{view.AccessCoin(schedule_key).out.scriptPubKey};
            if (count >= 278) BOOST_REQUIRE(original_schedule.size() > MAX_SCRIPT_SIZE);
            auto cursor{db.Cursor()};
            bool saw_schedule{false};
            while (cursor->Valid()) {
                COutPoint key;
                Coin coin;
                BOOST_REQUIRE(cursor->GetKey(key));
                BOOST_REQUIRE(cursor->GetValue(coin));
                if (key == schedule_key) {
                    saw_schedule = true;
                    BOOST_CHECK(coin.out.scriptPubKey == original_schedule);
                }
                cursor->Next();
            }
            BOOST_REQUIRE(saw_schedule);
            const int height{1 + node::recycle::EXPIRY_BLOCKS};
            CBlock expiry{CoinbaseBlock(50 * COIN + count, count + 1)};
            BOOST_REQUIRE_EQUAL(node::recycle::AvailableReward(view, expiry, height), count);
            AddCoins(view, *expiry.vtx[0], height);
            CBlockUndo block_undo;
            block_undo.vtxundo.emplace_back();
            std::string error;
            BOOST_REQUIRE(node::recycle::ConnectBlock(view, expiry, height, 50 * COIN, block_undo.vtxundo.back(), error));
            DataStream bytes;
            bytes << block_undo;
            CBlockUndo restored;
            bytes >> restored;
            BOOST_REQUIRE(restored.vtxundo.back().vprevout[0].out.scriptPubKey.size() > MAX_SCRIPT_SIZE);
            BOOST_REQUIRE(node::recycle::RollforwardBlock(view, expiry, height, 50 * COIN, &restored.vtxundo.back()));
            BOOST_REQUIRE(node::recycle::DisconnectBlock(view, height, &restored.vtxundo.back()));
            BOOST_CHECK(view.AccessCoin(schedule_key).out.scriptPubKey == original_schedule);
            BOOST_CHECK(!view.HaveCoin(COutPoint{Txid{}, std::numeric_limits<uint32_t>::max()}));
            for (int n{0}; n < count; ++n) BOOST_CHECK(view.HaveCoin(COutPoint{origin.vtx[0]->GetHash(), static_cast<uint32_t>(n)}));
        }
    }
}

BOOST_AUTO_TEST_CASE(repair_truncated_schedule_from_block)
{
    CCoinsView base;
    CCoinsViewCache view{&base};
    CBlock origin{CoinbaseBlock(COIN, 100)};
    CMutableTransaction tx{*origin.vtx[0]};
    tx.vout.assign(300, CTxOut{1, CScript{} << OP_TRUE});
    origin.vtx[0] = MakeTransactionRef(std::move(tx));
    AddCoins(view, *origin.vtx[0], 1);
    CTxUndo undo;
    std::string error;
    BOOST_REQUIRE(node::recycle::ConnectBlock(view, origin, 1, COIN, undo, error));
    const COutPoint key{Txid{}, 1'000'001};
    const auto expected{view.AccessCoin(key).out.scriptPubKey};
    Coin damaged{view.AccessCoin(key)};
    damaged.out.scriptPubKey = CScript{OP_RETURN};
    view.SpendCoin(key);
    view.AddCoin(key, std::move(damaged), false, true);
    BOOST_REQUIRE(!node::recycle::ScheduleValid(view, 1));
    node::recycle::RestoreSchedule(view, origin, 1);
    BOOST_CHECK(view.AccessCoin(key).out.scriptPubKey == expected);
}

BOOST_AUTO_TEST_CASE(metadata_reader_bounds_and_truncation)
{
    CScript decoded;
    DataStream oversized;
    const uint32_t size{static_cast<uint32_t>(MAX_SIZE) + 1 + ScriptCompression::nSpecialScripts};
    oversized << VARINT(size);
    BOOST_CHECK_THROW((oversized >> Using<node::recycle::MetadataScriptCompression>(decoded)), std::ios_base::failure);

    // The long-script exception applies only to internal OP_RETURN records.
    CScript non_metadata;
    non_metadata.resize(MAX_SCRIPT_SIZE + 1);
    non_metadata.front() = OP_TRUE;
    DataStream non_metadata_stream;
    non_metadata_stream << Using<ScriptCompression>(non_metadata);
    BOOST_CHECK_THROW((non_metadata_stream >> Using<node::recycle::MetadataScriptCompression>(decoded)), std::ios_base::failure);

    DataStream truncated;
    const uint32_t missing_payload{100 + ScriptCompression::nSpecialScripts};
    truncated << VARINT(missing_payload);
    BOOST_CHECK_THROW((truncated >> Using<node::recycle::MetadataScriptCompression>(decoded)), std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(missing_schedule_is_not_zero_reward)
{
    CCoinsView base;
    CCoinsViewCache view{&base};
    const CBlock block{CoinbaseBlock(COIN, 101)};
    BOOST_CHECK_THROW(node::recycle::AvailableReward(view, block, node::recycle::EXPIRY_BLOCKS + 1), std::runtime_error);
    CTxUndo undo;
    std::string error;
    BOOST_CHECK(!node::recycle::ConnectBlock(view, block, node::recycle::EXPIRY_BLOCKS + 1, COIN, undo, error));
    BOOST_CHECK_EQUAL(error, "missing or corrupt recycle expiry record");
}

BOOST_AUTO_TEST_SUITE_END()
