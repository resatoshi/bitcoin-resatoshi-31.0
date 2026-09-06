// Copyright (c) 2020 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_POW_ASERT_H
#define BITCOIN_POW_ASERT_H

#include <cstdint>

class arith_uint256;

/** Calculate the aserti3-2d target using fixed-point integer arithmetic. */
arith_uint256 CalculateASERT(const arith_uint256& ref_target, int64_t target_spacing,
                            int64_t time_diff, int64_t height_diff,
                            const arith_uint256& pow_limit, int64_t half_life) noexcept;

#endif // BITCOIN_POW_ASERT_H
