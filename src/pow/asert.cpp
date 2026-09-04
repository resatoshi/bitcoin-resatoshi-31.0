// Copyright (c) 2020 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <pow/asert.h>

#include <arith_uint256.h>

#include <cassert>
#include <cstdint>

arith_uint256 CalculateASERT(const arith_uint256& ref_target, const int64_t target_spacing,
                            const int64_t time_diff, const int64_t height_diff,
                            const arith_uint256& pow_limit, const int64_t half_life) noexcept
{
    assert(ref_target > 0 && ref_target <= pow_limit);
    assert((pow_limit >> 224) == 0);
    assert(height_diff >= 0);
    assert(target_spacing > 0 && half_life > 0);

    const int64_t exponent = ((time_diff - target_spacing * (height_diff + 1)) * 65536) / half_life;
    static_assert(int64_t{-1} >> 1 == int64_t{-1}, "ASERT requires arithmetic right shift");

    int64_t shifts = exponent >> 16;
    const uint16_t frac = static_cast<uint16_t>(exponent);

    // Cubic approximation of 2^x for 0 <= x < 1, scaled by 2^16.
    const uint32_t factor =
        65536 + ((195766423245049ULL * frac + 971821376ULL * frac * frac +
                  5127ULL * frac * frac * frac + (1ULL << 47)) >> 48);

    arith_uint256 next_target = ref_target * factor;
    shifts -= 16;
    if (shifts <= 0) {
        next_target >>= -shifts;
    } else {
        const arith_uint256 shifted = next_target << shifts;
        next_target = (shifted >> shifts) != next_target ? pow_limit : shifted;
    }

    if (next_target == 0) return arith_uint256{1};
    return next_target > pow_limit ? pow_limit : next_target;
}
