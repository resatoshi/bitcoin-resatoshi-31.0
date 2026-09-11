// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/recycle.h>
#include <node/recycle_serialization.h>

#include <coins.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <script/script.h>
#include <streams.h>
#include <undo.h>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace node::recycle {
namespace {

const Txid& ReservedTxid()
{
    static const Txid txid{};
    return txid;
}

COutPoint PoolOutpoint()
{
    return COutPoint{ReservedTxid(), std::numeric_limits<uint32_t>::max()};
}

COutPoint ScheduleOutpoint(int height)
{
    static constexpr uint32_t SCHEDULE_OFFSET{1'000'000};
    return COutPoint{ReservedTxid(), SCHEDULE_OFFSET + static_cast<uint32_t>(height)};
}

struct UndoData {
    CAmount pool_before{0};
    Coin schedule;
    std::vector<std::pair<COutPoint, Coin>> expired;

    SERIALIZE_METHODS(UndoData, obj) { READWRITE(obj.pool_before, Using<StateCoinFormatter>(obj.schedule), obj.expired); }
};

CScript Encode(const auto& value)
{
    DataStream stream{};
    stream << value;
    CScript script{OP_RETURN};
    script.insert(script.end(), UCharCast(stream.data()), UCharCast(stream.data() + stream.size()));
    return script;
}

template <typename T>
bool Decode(const CScript& script, T& value)
{
    if (script.empty() || script[0] != OP_RETURN) return false;
    try {
        DataStream stream{std::span{script}.subspan(1)};
        stream >> value;
        return stream.empty();
    } catch (const std::exception&) {
        return false;
    }
}

Coin StateCoin(CAmount amount, CScript script, int height)
{
    return Coin{CTxOut{amount, std::move(script)}, height, false};
}

void PutStateCoin(CCoinsViewCache& view, const COutPoint& outpoint, Coin coin)
{
    view.SpendCoin(outpoint);
    view.AddCoin(outpoint, std::move(coin), /*possible_overwrite=*/false,
                 /*allow_unspendable=*/true);
}

std::vector<COutPoint> CurrentOutputs(const CCoinsViewCache& view, const CBlock& block, int height)
{
    std::vector<COutPoint> outputs;
    for (const auto& tx : block.vtx) {
        for (uint32_t n = 0; n < tx->vout.size(); ++n) {
            COutPoint outpoint{tx->GetHash(), n};
            const Coin& coin{view.AccessCoin(outpoint)};
            if (!coin.IsSpent() && coin.nHeight == height) outputs.push_back(outpoint);
        }
    }
    return outputs;
}

bool ReadSchedule(const CCoinsViewCache& view, int height, Coin& schedule, std::vector<COutPoint>& outputs)
{
    const auto coin{view.GetCoin(ScheduleOutpoint(height))};
    if (!coin || !Decode(coin->out.scriptPubKey, outputs)) return false;
    schedule = *coin;
    return true;
}

CAmount ExpiringValue(const CCoinsViewCache& view, const CBlock& block, int height, int expiry_blocks)
{
    const int origin{height - expiry_blocks};
    if (origin < 1) return 0;

    Coin schedule;
    std::vector<COutPoint> outputs;
    if (!ReadSchedule(view, origin, schedule, outputs)) {
        throw std::runtime_error("missing or corrupt recycle expiry record; rebuild chainstate from original blocks");
    }

    std::set<COutPoint> spent;
    for (const auto& tx : block.vtx) {
        if (tx && !tx->IsCoinBase()) {
            for (const auto& input : tx->vin) spent.insert(input.prevout);
        }
    }

    CAmount amount{0};
    for (const auto& outpoint : outputs) {
        const Coin& coin{view.AccessCoin(outpoint)};
        if (!coin.IsSpent() && coin.nHeight == origin && !spent.contains(outpoint)) amount += coin.out.nValue;
    }
    return amount;
}

bool Apply(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
           CTxUndo* undo, std::string& error, std::vector<COutPoint>* expired_outpoints, int expiry_blocks)
{
    UndoData undo_data;
    undo_data.pool_before = PoolBalance(view);

    CAmount expired_value{0};
    const int origin{height - expiry_blocks};
    if (origin < 1) {
        if (block.vtx[0]->GetValueOut() > base_reward) {
            error = "coinbase claims recycle reward before any output can expire";
            return false;
        }
        PutStateCoin(view, ScheduleOutpoint(height),
                     StateCoin(0, Encode(CurrentOutputs(view, block, height)), height));
        return true;
    }
    {
        std::vector<COutPoint> outputs;
        if (!ReadSchedule(view, origin, undo_data.schedule, outputs)) {
            error = "missing or corrupt recycle expiry record";
            return false;
        }
        view.SpendCoin(ScheduleOutpoint(origin));
        for (const auto& outpoint : outputs) {
            const Coin& coin{view.AccessCoin(outpoint)};
            if (!coin.IsSpent() && coin.nHeight == origin) {
                Coin expired;
                if (!view.SpendCoin(outpoint, &expired)) return false;
                expired_value += expired.out.nValue;
                if (expired_outpoints) expired_outpoints->push_back(outpoint);
                undo_data.expired.emplace_back(outpoint, std::move(expired));
            }
        }
    }

    const CAmount available{undo_data.pool_before + expired_value};
    const CAmount claimed{std::max<CAmount>(0, block.vtx[0]->GetValueOut() - base_reward)};
    if (claimed > std::min(MAX_REWARD, available)) {
        error = "coinbase claims excessive recycle reward";
        return false;
    }

    PutStateCoin(view, PoolOutpoint(), StateCoin(available - claimed, CScript{OP_RETURN}, height));
    PutStateCoin(view, ScheduleOutpoint(height),
                 StateCoin(0, Encode(CurrentOutputs(view, block, height)), height));

    if (undo) undo->vprevout.emplace_back(StateCoin(0, Encode(undo_data), height));
    return true;
}

} // namespace

CAmount PoolBalance(const CCoinsViewCache& view)
{
    const Coin& coin{view.AccessCoin(PoolOutpoint())};
    return coin.IsSpent() ? 0 : coin.out.nValue;
}

CAmount AvailableReward(const CCoinsViewCache& view, const CBlock& block, int height, int expiry_blocks)
{
    return std::min(MAX_REWARD, PoolBalance(view) + ExpiringValue(view, block, height, expiry_blocks));
}

bool ConnectBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                  CTxUndo& undo, std::string& error, std::vector<COutPoint>* expired, int expiry_blocks)
{
    return Apply(view, block, height, base_reward, &undo, error, expired, expiry_blocks);
}

bool DisconnectBlock(CCoinsViewCache& view, int height, CTxUndo* undo, int expiry_blocks)
{
    view.SpendCoin(ScheduleOutpoint(height));
    if (height - expiry_blocks < 1) return true;
    if (!undo || undo->vprevout.size() != 1) return false;
    UndoData data;
    if (!Decode(undo->vprevout[0].out.scriptPubKey, data)) return false;

    if (height - expiry_blocks == 1) {
        view.SpendCoin(PoolOutpoint());
    } else {
        PutStateCoin(view, PoolOutpoint(), StateCoin(data.pool_before, CScript{OP_RETURN}, height - 1));
    }
    view.AddCoin(ScheduleOutpoint(height - expiry_blocks), std::move(data.schedule),
                 /*possible_overwrite=*/false, /*allow_unspendable=*/true);
    for (auto& [outpoint, coin] : data.expired) view.AddCoin(outpoint, std::move(coin), false);
    return true;
}

bool RollforwardBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                      const CTxUndo* undo, int expiry_blocks)
{
    const int origin{height - expiry_blocks};
    if (origin < 1) {
        std::string error;
        return Apply(view, block, height, base_reward, nullptr, error, nullptr, expiry_blocks);
    }
    if (!undo || undo->vprevout.size() != 1) return false;

    UndoData data;
    if (!Decode(undo->vprevout[0].out.scriptPubKey, data)) return false;
    CAmount expired_value{0};
    for (const auto& [outpoint, coin] : data.expired) expired_value += coin.out.nValue;

    const CAmount available{data.pool_before + expired_value};
    const CAmount claimed{std::max<CAmount>(0, block.vtx[0]->GetValueOut() - base_reward)};
    if (claimed > std::min(MAX_REWARD, available)) return false;

    view.SpendCoin(ScheduleOutpoint(origin));
    for (const auto& [outpoint, coin] : data.expired) view.SpendCoin(outpoint);
    PutStateCoin(view, PoolOutpoint(), StateCoin(available - claimed, CScript{OP_RETURN}, height));
    PutStateCoin(view, ScheduleOutpoint(height),
                 StateCoin(0, Encode(CurrentOutputs(view, block, height)), height));
    return true;
}

bool ReadUndo(const CTxUndo& undo, CAmount& pool_before, std::vector<std::pair<COutPoint, Coin>>& expired)
{
    UndoData data;
    if (undo.vprevout.size() != 1 || !Decode(undo.vprevout[0].out.scriptPubKey, data)) return false;
    pool_before = data.pool_before;
    expired = std::move(data.expired);
    return true;
}

bool ScheduleValid(const CCoinsViewCache& view, int height)
{
    Coin schedule;
    std::vector<COutPoint> outputs;
    return ReadSchedule(view, height, schedule, outputs) && schedule.nHeight == height;
}

void RestoreSchedule(CCoinsViewCache& view, const CBlock& block, int height)
{
    // Reproduce outputs surviving the original block, including those spent
    // later. No dependency on today's UTXO contents or a wallet is needed.
    std::set<COutPoint> spent;
    for (const auto& tx : block.vtx) {
        if (!tx->IsCoinBase()) for (const auto& input : tx->vin) spent.insert(input.prevout);
    }
    std::vector<COutPoint> outputs;
    for (const auto& tx : block.vtx) {
        for (uint32_t n = 0; n < tx->vout.size(); ++n) {
            COutPoint outpoint{tx->GetHash(), n};
            if (!tx->vout[n].scriptPubKey.IsUnspendable() && !spent.contains(outpoint)) outputs.push_back(outpoint);
        }
    }
    PutStateCoin(view, ScheduleOutpoint(height), StateCoin(0, Encode(outputs), height));
}

} // namespace node::recycle
