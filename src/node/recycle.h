// Copyright (c) 2026 The Bitcoin ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_RECYCLE_H
#define BITCOIN_NODE_RECYCLE_H

#include <consensus/amount.h>
#include <primitives/transaction.h>

#include <cstdint>
#include <string>
#include <vector>

class CBlock;
class CCoinsViewCache;
class CTxUndo;

namespace node::recycle {

inline constexpr int EXPIRY_BLOCKS{5'256'000};
inline constexpr CAmount MAX_REWARD{COIN};

/** Return the recycle pool recorded in the supplied chainstate view. */
CAmount PoolBalance(const CCoinsViewCache& view);

/** Return the maximum recycle reward a candidate block may claim. */
CAmount AvailableReward(const CCoinsViewCache& view, const CBlock& block, int height);

/** Apply expiry, pool accounting, and creation of the current height's expiry record. */
bool ConnectBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                  CTxUndo& undo, std::string& error, std::vector<COutPoint>* expired = nullptr);

/** Undo ConnectBlock. */
bool DisconnectBlock(CCoinsViewCache& view, int height, CTxUndo* undo);

/** Reapply the state transition while recovering an interrupted chainstate flush. */
bool RollforwardBlock(CCoinsViewCache& view, const CBlock& block, int height, CAmount base_reward,
                      const CTxUndo* undo);

} // namespace node::recycle

#endif // BITCOIN_NODE_RECYCLE_H
