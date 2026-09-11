// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_CPUMINERHASH_H
#define BITCOIN_QT_CPUMINERHASH_H

#include <crypto/common.h>
#include <crypto/sha256.h>
#include <primitives/block.h>
#include <streams.h>

#include <algorithm>
#include <array>

/** Reuse the first SHA256 compression block of an unchanged 80-byte header. */
class CpuMinerHasher
{
public:
    explicit CpuMinerHasher(const CBlockHeader& header)
    {
        DataStream stream;
        stream << header;
        const auto* bytes = reinterpret_cast<const unsigned char*>(stream.data());
        m_prefix.Write(bytes, 64);
        std::copy_n(bytes + 64, m_tail.size(), m_tail.begin());
    }

    uint256 hash(uint32_t nonce)
    {
        WriteLE32(m_tail.data() + 12, nonce);
        auto first{m_prefix};
        std::array<unsigned char, CSHA256::OUTPUT_SIZE> intermediate;
        first.Write(m_tail.data(), m_tail.size()).Finalize(intermediate.data());
        uint256 result;
        CSHA256{}.Write(intermediate.data(), intermediate.size()).Finalize(result.begin());
        return result;
    }

private:
    CSHA256 m_prefix;
    std::array<unsigned char, 16> m_tail;
};

#endif // BITCOIN_QT_CPUMINERHASH_H
