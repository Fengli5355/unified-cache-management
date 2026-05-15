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
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "distributed_hash_table.h"

namespace {

using UC::AsuStore::Crc32IEEE;
using UC::AsuStore::CreateDistributedHashTable;
using UC::AsuStore::DistributedHashTable;
using UC::AsuStore::HashTableConfig;
using UC::AsuStore::HashTableType;
using UC::AsuStore::KVBuffer;
using UC::AsuStore::Maglev;
using UC::AsuStore::RingHashTable;
using UC::AsuStore::U64Max;

struct RingHashTestConfig {
    uint64_t virtualNodeCount{1};
    std::vector<uint64_t> asuIds;
    std::vector<UC::AsuStore::CacheKey> keys;
    std::vector<uint64_t> expectedAsuIds;
    std::unordered_map<std::string, uint64_t> hashValues;
};

std::vector<std::string> Split(const std::string& value, char delimiter)
{
    std::vector<std::string> parts;
    std::stringstream stream{value};
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        if (!part.empty()) { parts.emplace_back(part); }
    }
    return parts;
}

std::string GetCurrentDirFromFile(const std::string& file)
{
    auto pos = file.find_last_of("/\\");
    if (pos == std::string::npos) { return "."; }
    return file.substr(0, pos);
}

RingHashTestConfig LoadRingHashTestConfig(const std::string& path)
{
    std::ifstream configFile{path};
    if (!configFile.is_open()) { throw std::runtime_error("failed to open config file: " + path); }

    RingHashTestConfig config;
    std::string line;
    while (std::getline(configFile, line)) {
        if (line.empty() || line[0] == '#') { continue; }

        auto pos = line.find('=');
        if (pos == std::string::npos) { continue; }

        const auto key = line.substr(0, pos);
        const auto value = line.substr(pos + 1);
        if (key == "virtual_node_count") {
            config.virtualNodeCount = std::stoull(value);
        } else if (key == "asu_ids") {
            for (const auto& asuId : Split(value, ',')) {
                config.asuIds.emplace_back(std::stoull(asuId));
            }
        } else if (key == "hash") {
            auto parts = Split(value, ',');
            if (parts.size() == 2) { config.hashValues.emplace(parts[0], std::stoull(parts[1])); }
        } else if (key == "kv") {
            auto parts = Split(value, ',');
            if (parts.size() == 2) {
                config.keys.emplace_back(parts[0]);
                config.expectedAsuIds.emplace_back(std::stoull(parts[1]));
            }
        }
    }
    return config;
}

UC::AsuStore::RingHashFunction MakeHash(const std::unordered_map<std::string, uint64_t>& hashValues)
{
    return [hashValues](const std::string& key) {
        auto iter = hashValues.find(key);
        if (iter != hashValues.end()) { return iter->second; }
        return Crc32IEEE(key);
    };
}

void ExpectAsuIds(const std::vector<KVBuffer>& kvBuffers, const std::vector<uint64_t>& asuIds)
{
    ASSERT_EQ(kvBuffers.size(), asuIds.size());
    for (size_t i = 0; i < kvBuffers.size(); ++i) {
        EXPECT_EQ(kvBuffers[i].asuId, asuIds[i]) << "key: " << kvBuffers[i].key;
        EXPECT_EQ(kvBuffers[i].entryIdx, i) << "key: " << kvBuffers[i].key;
    }
}

void ExpectAsuIdsInSet(const std::vector<KVBuffer>& kvBuffers, const std::vector<uint64_t>& asuIds)
{
    for (size_t i = 0; i < kvBuffers.size(); ++i) {
        EXPECT_NE(std::find(asuIds.begin(), asuIds.end(), kvBuffers[i].asuId), asuIds.end())
            << "key: " << kvBuffers[i].key;
        EXPECT_EQ(kvBuffers[i].entryIdx, i) << "key: " << kvBuffers[i].key;
    }
}

}  // namespace

TEST(RingHashTableTest, DistributeInlineInput)
{
    std::unordered_map<std::string, uint64_t> hashValues{
        {"vn-0#asu-10", 10}, {"vn-0#asu-20", 20}, {"vn-0#asu-30", 30},
        {"key-05", 5},      {"key-10", 10},      {"key-15", 15},
        {"key-25", 25},     {"key-35", 35},
    };

    RingHashTable table{1, MakeHash(hashValues)};
    table.AddAsus({10, 20, 30});
    table.AddAsus({10});

    auto kvBuffers = table.DistributeToAsus({"key-05", "key-10", "key-15", "key-25", "key-35"});
    ExpectAsuIds(kvBuffers, {10, 10, 20, 30, 10});

    table.RemoveAsus({20});
    kvBuffers = table.DistributeToAsus({"key-15"});
    ExpectAsuIds(kvBuffers, {30});
}

TEST(RingHashTableTest, DistributeConfigInput)
{
    const auto configPath = GetCurrentDirFromFile(__FILE__) + "/distributed_hash_table_test.config";
    auto config = LoadRingHashTestConfig(configPath);

    RingHashTable table{config.virtualNodeCount, MakeHash(config.hashValues)};
    table.AddAsus(config.asuIds);
    auto kvBuffers = table.DistributeToAsus(config.keys);
    ExpectAsuIds(kvBuffers, config.expectedAsuIds);
}

TEST(RingHashTableTest, KeepAsuIdWhenRingIsEmpty)
{
    RingHashTable table;

    auto kvBuffers = table.DistributeToAsus({"key-01"});

    ASSERT_EQ(kvBuffers.size(), size_t{1});
    EXPECT_EQ(kvBuffers[0].asuId, U64Max);
}

TEST(RingHashTableTest, DistributeWithDefaultHash)
{
    RingHashTable table{3};
    table.AddAsus({10, 20, 30});

    auto kvBuffers = table.DistributeToAsus({"key-0", "key-1", "key-2", "key-3"});
    ASSERT_EQ(kvBuffers.size(), size_t{4});
    ExpectAsuIdsInSet(kvBuffers, {10, 20, 30});

    table.RemoveAsus({10, 30});
    kvBuffers = table.DistributeToAsus({"key-0", "key-1", "key-2", "key-3"});
    ExpectAsuIds(kvBuffers, {20, 20, 20, 20});
}

TEST(MaglevTest, DistributeToLookupTable)
{
    Maglev table{7, MakeHash({
                         {"maglev-offset#asu-10", 0},
                         {"maglev-skip#asu-10", 0},
                         {"maglev-offset#asu-20", 1},
                         {"maglev-skip#asu-20", 0},
                         {"key-0", 0},
                         {"key-1", 1},
                         {"key-2", 2},
                     })};

    table.AddAsus({10, 20});

    auto kvBuffers = table.DistributeToAsus({"key-0", "key-1", "key-2"});
    ExpectAsuIds(kvBuffers, {10, 20, 10});

    table.RemoveAsus({10});
    kvBuffers = table.DistributeToAsus({"key-0", "key-1", "key-2"});
    ExpectAsuIds(kvBuffers, {20, 20, 20});
}

TEST(MaglevTest, DistributeWithDefaultHash)
{
    Maglev table{17};
    table.AddAsus({10, 20, 30});

    auto kvBuffers = table.DistributeToAsus({"key-0", "key-1", "key-2", "key-3"});
    ASSERT_EQ(kvBuffers.size(), size_t{4});
    ExpectAsuIdsInSet(kvBuffers, {10, 20, 30});

    table.RemoveAsus({10, 20});
    kvBuffers = table.DistributeToAsus({"key-0", "key-1", "key-2", "key-3"});
    ExpectAsuIds(kvBuffers, {30, 30, 30, 30});
}

TEST(DistributedHashTableTest, CreateConfiguredHashTable)
{
    HashTableConfig config;
    config.type = HashTableType::MAGLEV;
    config.maglevTableSize = 7;

    auto table = CreateDistributedHashTable(config, MakeHash({
                                                        {"maglev-offset#asu-10", 0},
                                                        {"maglev-skip#asu-10", 0},
                                                        {"key-0", 0},
                                                    }));
    table->AddAsus({10});

    auto kvBuffers = table->DistributeToAsus({"key-0"});

    ExpectAsuIds(kvBuffers, {10});
}

TEST(DistributedHashTableTest, CreateConfiguredHashTableWithDefaultHash)
{
    HashTableConfig config;
    config.type = HashTableType::RING_HASH;
    config.virtualNodeCount = 3;

    auto table = CreateDistributedHashTable(config);
    table->AddAsus({10, 20});

    auto kvBuffers = table->DistributeToAsus({"key-0", "key-1"});

    ASSERT_EQ(kvBuffers.size(), size_t{2});
    ExpectAsuIdsInSet(kvBuffers, {10, 20});
}
