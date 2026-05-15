/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#ifndef UNIFIEDCACHE_ASUSTORE_DISTRIBUTED_HASH_TABLE_H
#define UNIFIEDCACHE_ASUSTORE_DISTRIBUTED_HASH_TABLE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "transport_client.h"

namespace UC::AsuStore {

using RingNode = std::pair<uint64_t, uint64_t>;  // hash value -> asu id.
using RingData = std::vector<RingNode>;
using HashFunction = std::function<uint64_t(const std::string&)>;
using RingHashFunction = HashFunction;

static constexpr uint64_t kDefaultVirtualNodeCount = 128;
static constexpr uint64_t kDefaultMaglevTableSize = 65537;

enum class HashTableType {
    RING_HASH = 0,
    MAGLEV = 1,
};

struct HashTableConfig {
    HashTableType type{HashTableType::RING_HASH};
    uint64_t virtualNodeCount{kDefaultVirtualNodeCount};
    uint64_t maglevTableSize{kDefaultMaglevTableSize};
};

uint64_t Crc32IEEE(const std::string& data);

class DistributedHashTable {
public:
    virtual ~DistributedHashTable() = default;

    virtual void AddAsus(const std::vector<uint64_t>& asuIds) = 0;
    virtual void RemoveAsus(const std::vector<uint64_t>& asuIds) = 0;
    virtual std::vector<KVBuffer> DistributeToAsus(const std::vector<CacheKey>& keys) const = 0;
};

class RingHashTable : public DistributedHashTable {
public:
    explicit RingHashTable(uint64_t virtualNodeCount = kDefaultVirtualNodeCount,
                           HashFunction hash = Crc32IEEE);
    ~RingHashTable() override = default;

    void AddAsus(const std::vector<uint64_t>& asuIds) override;
    void RemoveAsus(const std::vector<uint64_t>& asuIds) override;
    std::vector<KVBuffer> DistributeToAsus(const std::vector<CacheKey>& keys) const override;

private:
    void InsertAsuVirtualNode(RingData& ringData, uint64_t asuId, uint64_t index) const;
    static std::string BuildVirtualNodeKey(uint64_t asuId, uint64_t index, uint64_t salt);
    static bool HasHash(const RingData& ringData, uint64_t hashValue);
    static bool HasAsu(const RingData& ringData, uint64_t asuId);

    HashFunction hash_;
    uint64_t virtualNodeCount_;
    mutable std::mutex ringMutex_;
    std::shared_ptr<const RingData> ring_;
};

class Maglev : public DistributedHashTable {
public:
    explicit Maglev(uint64_t tableSize = kDefaultMaglevTableSize, HashFunction hash = Crc32IEEE);
    ~Maglev() override = default;

    void AddAsus(const std::vector<uint64_t>& asuIds) override;
    void RemoveAsus(const std::vector<uint64_t>& asuIds) override;
    std::vector<KVBuffer> DistributeToAsus(const std::vector<CacheKey>& keys) const override;

private:
    struct MaglevPermutation {
        uint64_t offset{0};
        uint64_t skip{1};
    };

    std::shared_ptr<const std::vector<uint64_t>> BuildLookupTable(
        const std::vector<uint64_t>& asuIds) const;
    MaglevPermutation BuildPermutation(uint64_t asuId) const;
    static bool HasAsu(const std::vector<uint64_t>& asuIds, uint64_t asuId);

    HashFunction hash_;
    uint64_t tableSize_;
    mutable std::mutex tableMutex_;
    std::vector<uint64_t> asuIds_;
    std::shared_ptr<const std::vector<uint64_t>> lookupTable_;
};

using MaglevHashTable = Maglev;

std::unique_ptr<DistributedHashTable> CreateDistributedHashTable(const HashTableConfig& config,
                                                                 HashFunction hash = Crc32IEEE);

}  // namespace UC::AsuStore

#endif
