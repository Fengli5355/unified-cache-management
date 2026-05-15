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
#include "distributed_hash_table.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace UC::AsuStore {
namespace {

std::vector<KVBuffer> MakeEmptyKvBuffers(const std::vector<CacheKey>& keys)
{
    std::vector<KVBuffer> kvBuffers;
    kvBuffers.reserve(keys.size());
    for (size_t entryIdx = 0; entryIdx < keys.size(); ++entryIdx) {
        KVBuffer kvBuffer;
        kvBuffer.key = keys[entryIdx];
        kvBuffer.entryIdx = entryIdx;
        kvBuffers.emplace_back(std::move(kvBuffer));
    }
    return kvBuffers;
}

bool IsPrime(uint64_t value)
{
    if (value < 2) { return false; }
    if (value == 2) { return true; }
    if (value % 2 == 0) { return false; }
    for (uint64_t factor = 3; factor <= value / factor; factor += 2) {
        if (value % factor == 0) { return false; }
    }
    return true;
}

}  // namespace

uint64_t Crc32IEEE(const std::string& data)
{
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if ((crc & 1U) != 0) {
                    crc = 0xEDB88320U ^ (crc >> 1U);
                } else {
                    crc >>= 1U;
                }
            }
            values[i] = crc;
        }
        return values;
    }();

    uint32_t crc = 0xFFFFFFFFU;
    for (unsigned char ch : data) {
        crc = table[(crc ^ ch) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

RingHashTable::RingHashTable(uint64_t virtualNodeCount, HashFunction hash)
    : hash_(std::move(hash)),
      virtualNodeCount_(virtualNodeCount),
      ring_(std::make_shared<RingData>())
{
}

void RingHashTable::AddAsus(const std::vector<uint64_t>& asuIds)
{
    std::lock_guard<std::mutex> lock{ringMutex_};
    auto ringData = std::make_shared<RingData>(*std::atomic_load(&ring_));

    for (auto asuId : asuIds) {
        if (asuId == U64Max || HasAsu(*ringData, asuId)) { continue; }

        for (uint64_t index = 0; index < virtualNodeCount_; ++index) {
            InsertAsuVirtualNode(*ringData, asuId, index);
        }
    }

    std::sort(ringData->begin(), ringData->end());
    std::shared_ptr<const RingData> readonlyRingData = std::move(ringData);
    std::atomic_store(&ring_, readonlyRingData);
}

void RingHashTable::RemoveAsus(const std::vector<uint64_t>& asuIds)
{
    std::lock_guard<std::mutex> lock{ringMutex_};
    auto ringData = std::make_shared<RingData>(*std::atomic_load(&ring_));

    for (auto asuId : asuIds) {
        ringData->erase(std::remove_if(ringData->begin(), ringData->end(),
                                       [asuId](const RingNode& ringNode) {
                                           return ringNode.second == asuId;
                                       }),
                        ringData->end());
    }

    std::shared_ptr<const RingData> readonlyRingData = std::move(ringData);
    std::atomic_store(&ring_, readonlyRingData);
}

std::vector<KVBuffer> RingHashTable::DistributeToAsus(const std::vector<CacheKey>& keys) const
{
    auto kvBuffers = MakeEmptyKvBuffers(keys);
    auto ringData = std::atomic_load(&ring_);
    if (ringData->empty()) { return kvBuffers; }

    for (auto& kvBuffer : kvBuffers) {
        auto iter = std::lower_bound(ringData->begin(), ringData->end(), hash_(kvBuffer.key),
                                     [](const RingNode& ringNode, uint64_t hashValue) {
                                         return ringNode.first < hashValue;
                                     });
        if (iter == ringData->end()) { iter = ringData->begin(); }
        kvBuffer.asuId = iter->second;
    }

    return kvBuffers;
}

void RingHashTable::InsertAsuVirtualNode(RingData& ringData, uint64_t asuId, uint64_t index) const
{
    uint64_t salt = 0;
    auto hashValue = hash_(BuildVirtualNodeKey(asuId, index, salt));
    while (HasHash(ringData, hashValue)) {
        ++salt;
        hashValue = hash_(BuildVirtualNodeKey(asuId, index, salt));
    }

    ringData.emplace_back(hashValue, asuId);
}

std::string RingHashTable::BuildVirtualNodeKey(uint64_t asuId, uint64_t index, uint64_t salt)
{
    auto key = "vn-" + std::to_string(index) + "#asu-" + std::to_string(asuId);
    if (salt == 0) { return key; }
    return key + "#" + std::to_string(salt);
}

bool RingHashTable::HasHash(const RingData& ringData, uint64_t hashValue)
{
    return std::any_of(ringData.begin(), ringData.end(), [hashValue](const RingNode& ringNode) {
        return ringNode.first == hashValue;
    });
}

bool RingHashTable::HasAsu(const RingData& ringData, uint64_t asuId)
{
    return std::any_of(ringData.begin(), ringData.end(), [asuId](const RingNode& ringNode) {
        return ringNode.second == asuId;
    });
}

Maglev::Maglev(uint64_t tableSize, HashFunction hash)
    : hash_(std::move(hash)),
      tableSize_(tableSize),
      lookupTable_(std::make_shared<std::vector<uint64_t>>())
{
    if (!IsPrime(tableSize_)) { throw std::invalid_argument("maglev table size must be prime"); }
}

void Maglev::AddAsus(const std::vector<uint64_t>& asuIds)
{
    std::lock_guard<std::mutex> lock{tableMutex_};
    auto nextAsuIds = asuIds_;
    for (auto asuId : asuIds) {
        if (asuId == U64Max || HasAsu(nextAsuIds, asuId)) { continue; }
        nextAsuIds.emplace_back(asuId);
    }

    auto lookupTable = BuildLookupTable(nextAsuIds);
    asuIds_ = std::move(nextAsuIds);
    std::atomic_store(&lookupTable_, lookupTable);
}

void Maglev::RemoveAsus(const std::vector<uint64_t>& asuIds)
{
    std::lock_guard<std::mutex> lock{tableMutex_};
    auto nextAsuIds = asuIds_;
    for (auto asuId : asuIds) {
        nextAsuIds.erase(std::remove(nextAsuIds.begin(), nextAsuIds.end(), asuId), nextAsuIds.end());
    }

    auto lookupTable = BuildLookupTable(nextAsuIds);
    asuIds_ = std::move(nextAsuIds);
    std::atomic_store(&lookupTable_, lookupTable);
}

std::vector<KVBuffer> Maglev::DistributeToAsus(const std::vector<CacheKey>& keys) const
{
    auto kvBuffers = MakeEmptyKvBuffers(keys);
    auto lookupTable = std::atomic_load(&lookupTable_);
    if (lookupTable->empty()) { return kvBuffers; }

    for (auto& kvBuffer : kvBuffers) {
        kvBuffer.asuId = (*lookupTable)[hash_(kvBuffer.key) % lookupTable->size()];
    }
    return kvBuffers;
}

std::shared_ptr<const std::vector<uint64_t>> Maglev::BuildLookupTable(
    const std::vector<uint64_t>& asuIds) const
{
    auto lookupTable = std::make_shared<std::vector<uint64_t>>();
    if (asuIds.empty()) { return lookupTable; }

    lookupTable->assign(tableSize_, U64Max);
    std::vector<MaglevPermutation> permutations;
    std::vector<uint64_t> next;
    permutations.reserve(asuIds.size());
    next.assign(asuIds.size(), 0);

    for (auto asuId : asuIds) {
        permutations.emplace_back(BuildPermutation(asuId));
    }

    uint64_t filled = 0;
    while (filled < tableSize_) {
        for (size_t index = 0; index < asuIds.size() && filled < tableSize_; ++index) {
            uint64_t candidate =
                (permutations[index].offset + next[index] * permutations[index].skip) % tableSize_;
            ++next[index];
            while ((*lookupTable)[candidate] != U64Max) {
                candidate =
                    (permutations[index].offset + next[index] * permutations[index].skip) % tableSize_;
                ++next[index];
            }

            (*lookupTable)[candidate] = asuIds[index];
            ++filled;
        }
    }

    return lookupTable;
}

Maglev::MaglevPermutation Maglev::BuildPermutation(uint64_t asuId) const
{
    const auto value = std::to_string(asuId);
    MaglevPermutation permutation;
    permutation.offset = hash_("maglev-offset#asu-" + value) % tableSize_;
    if (tableSize_ > 1) {
        permutation.skip = hash_("maglev-skip#asu-" + value) % (tableSize_ - 1) + 1;
    }
    return permutation;
}

bool Maglev::HasAsu(const std::vector<uint64_t>& asuIds, uint64_t asuId)
{
    return std::find(asuIds.begin(), asuIds.end(), asuId) != asuIds.end();
}

std::unique_ptr<DistributedHashTable> CreateDistributedHashTable(const HashTableConfig& config,
                                                                 HashFunction hash)
{
    switch (config.type) {
        case HashTableType::RING_HASH:
            return std::make_unique<RingHashTable>(config.virtualNodeCount, std::move(hash));
        case HashTableType::MAGLEV:
            return std::make_unique<Maglev>(config.maglevTableSize, std::move(hash));
        default:
            throw std::invalid_argument("unknown distributed hash table type");
    }
}

}  // namespace UC::AsuStore
