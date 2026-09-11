// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_RECYCLE_SERIALIZATION_H
#define BITCOIN_NODE_RECYCLE_SERIALIZATION_H

#include <coins.h>
#include <compressor.h>
#include <serialize.h>

#include <span>

namespace node::recycle {

/** Internal state shares the coin cache's transactional/rollback semantics,
 * but is not a transaction script. Keep the existing disk bytes, and decode
 * this namespace without ScriptCompression's destructive script-size filter.
 */
inline bool IsStateOutpoint(const COutPoint& outpoint)
{
    return outpoint.hash.IsNull() && outpoint.n >= 1'000'001;
}

struct MetadataScriptCompression {
    template <typename Stream> void Ser(Stream& s, const CScript& script)
    {
        ScriptCompression{}.Ser(s, script);
    }

    template <typename Stream> void Unser(Stream& s, CScript& script)
    {
        unsigned int size{0};
        s >> VARINT(size);
        if (size < ScriptCompression::nSpecialScripts) {
            CompressedScript compressed(GetSpecialScriptSize(size), 0x00);
            s >> std::span{compressed};
            DecompressScript(script, size, compressed);
            return;
        }
        size -= ScriptCompression::nSpecialScripts;
        // Bound local database allocations too. Valid metadata from a block
        // fits within the serialization framework's 32 MiB limit.
        if (size > MAX_SIZE) throw std::ios_base::failure("oversized recycle metadata");
        script.resize(size);
        s >> std::span{script};
        if (size > MAX_SCRIPT_SIZE && script.front() != OP_RETURN) {
            throw std::ios_base::failure("oversized non-metadata script");
        }
    }
};

/** Byte-compatible with Coin, for reserved state records only. */
struct StateCoinFormatter {
    template <typename Stream> void Ser(Stream& s, const Coin& coin) { s << coin; }

    template <typename Stream> void Unser(Stream& s, Coin& coin)
    {
        uint32_t code{0};
        s >> VARINT(code);
        coin.nHeight = code >> 1;
        coin.fCoinBase = code & 1;
        s >> Using<AmountCompression>(coin.out.nValue);
        s >> Using<MetadataScriptCompression>(coin.out.scriptPubKey);
    }
};

} // namespace node::recycle

#endif // BITCOIN_NODE_RECYCLE_SERIALIZATION_H
