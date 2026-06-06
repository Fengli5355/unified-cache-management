/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
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
#include "client_config_parser.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>
#include "asu_client/asu_client.h"
#include "config_parser_common.h"
#include "status_utils.h"
#include "view_server.h"

namespace UC::ASU {
namespace {

struct ClientConfigParseContext {
    AsuClientConfig& config;
    std::unordered_map<AsuId, AsuInfo>& asuInfos;
};

using ClientConfigSetter = std::function<void(ClientConfigParseContext&, const std::string&)>;

AsuInfo ParseAsuInfo(const std::string& value)
{
    AsuInfo info;
    for (const auto& endpointValue : SplitConfigValue(value, ';')) {
        info.endpoints.emplace_back(ParseClientViewEndpoint(endpointValue));
    }
    return info;
}

void SetHashTableType(AsuClientConfig& config, std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (value == "MAGLEV" || value == "MAGLEV_FULL_SPREAD") {
        config.attrs["hash_table.type"] = "MAGLEV";
    } else if (value == "CONTIGUOUS_BLOCK_AFFINITY") {
        config.attrs["hash_table.type"] = "CONTIGUOUS_BLOCK_AFFINITY";
    } else if (value == "BATCH_TOPK_AFFINITY") {
        config.attrs["hash_table.type"] = "BATCH_TOPK_AFFINITY";
    } else {
        config.attrs["hash_table.type"] = "RING_HASH";
    }
}

void AddTransportConfigs(AsuClientConfig& config, const std::string& value)
{
    for (const auto& asuIdText : SplitConfigValue(value, ',')) {
        TransportConfig transportConfig;
        transportConfig.asuId = ParseConfigUint64(asuIdText);
        config.transportConfigs.emplace_back(std::move(transportConfig));
    }
}

// clang-format off
const std::unordered_map<std::string, ClientConfigSetter> g_clientConfigSetters = {
    {"clientId",                                                   [](ClientConfigParseContext& c, const std::string& v) { c.config.clientId = v; }},
    {"client_id",                                                  [](ClientConfigParseContext& c, const std::string& v) { c.config.clientId = v; }},
    {"viewServiceAddrs",                                           [](ClientConfigParseContext& c, const std::string& v) { c.config.viewServiceAddrs = SplitConfigValue(v, ','); }},
    {"view_service_addrs",                                         [](ClientConfigParseContext& c, const std::string& v) { c.config.viewServiceAddrs = SplitConfigValue(v, ','); }},
    {"view.config_path",                                           [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["view.config_path"] = v; }},
    {"viewConfigPath",                                             [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["view.config_path"] = v; }},
    {"view_config_path",                                           [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["view.config_path"] = v; }},
    {"defaultWaitTimeoutMs",                                       [](ClientConfigParseContext& c, const std::string& v) { c.config.defaultWaitTimeoutMs = ParseConfigUint64(v); }},
    {"default_wait_timeout_ms",                                    [](ClientConfigParseContext& c, const std::string& v) { c.config.defaultWaitTimeoutMs = ParseConfigUint64(v); }},
    {"router.type",                                                [](ClientConfigParseContext& c, const std::string& v) { SetHashTableType(c.config, v); }},
    {"routerType",                                                 [](ClientConfigParseContext& c, const std::string& v) { SetHashTableType(c.config, v); }},
    {"hashTable.type",                                             [](ClientConfigParseContext& c, const std::string& v) { SetHashTableType(c.config, v); }},
    {"hash_table.type",                                            [](ClientConfigParseContext& c, const std::string& v) { SetHashTableType(c.config, v); }},
    {"hashTable.ringHash.virtualNodeCount",                        [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["ring_hash.virtual_node_count"] = v; }},
    {"ring_hash.virtual_node_count",                               [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["ring_hash.virtual_node_count"] = v; }},
    {"hashTable.maglev.tableSize",                                 [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["maglev.table_size"] = v; }},
    {"maglev.table_size",                                          [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["maglev.table_size"] = v; }},
    {"hashTable.contiguousBlockAffinity.blockCount",               [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["contiguous_block_affinity.block_count"] = v; }},
    {"contiguous_block_affinity.block_count",                      [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["contiguous_block_affinity.block_count"] = v; }},
    {"hashTable.contiguousBlockAffinity.fullSpreadType",           [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["contiguous_block_affinity.full_spread_type"] = v; }},
    {"contiguous_block_affinity.full_spread_type",                 [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["contiguous_block_affinity.full_spread_type"] = v; }},
    {"hashTable.contiguousBlockAffinity.dynamicAdjustEnabled",     [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["contiguous_block_affinity.dynamic_adjust_enabled"] = v; }},
    {"contiguous_block_affinity.dynamic_adjust_enabled",           [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["contiguous_block_affinity.dynamic_adjust_enabled"] = v; }},
    {"hashTable.batchTopKAffinity.topK",                           [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["batch_topk_affinity.top_k"] = v; }},
    {"batch_topk_affinity.top_k",                                  [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["batch_topk_affinity.top_k"] = v; }},
    {"hashTable.batchTopKAffinity.dynamicAdjustEnabled",           [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["batch_topk_affinity.dynamic_adjust_enabled"] = v; }},
    {"batch_topk_affinity.dynamic_adjust_enabled",                 [](ClientConfigParseContext& c, const std::string& v) { c.config.attrs["batch_topk_affinity.dynamic_adjust_enabled"] = v; }},
    {"transport.asuIds",                                           [](ClientConfigParseContext& c, const std::string& v) { AddTransportConfigs(c.config, v); }},
    {"transport.asu_ids",                                          [](ClientConfigParseContext& c, const std::string& v) { AddTransportConfigs(c.config, v); }},
    {"asuIds",                                                     [](ClientConfigParseContext& c, const std::string& v) { AddTransportConfigs(c.config, v); }},
    {"asu_ids",                                                    [](ClientConfigParseContext& c, const std::string& v) { AddTransportConfigs(c.config, v); }},
};
// clang-format on

bool ApplyClientConfigField(ClientConfigParseContext& context, const std::string& key,
                            const std::string& value)
{
    const auto iter = g_clientConfigSetters.find(key);
    if (iter == g_clientConfigSetters.end()) { return false; }
    iter->second(context, value);
    return true;
}

}  // namespace

Status LoadAsuClientConfig(const std::string& configPath, AsuClientConfig& config)
{
    std::ifstream configFile{configPath};
    if (!configFile.is_open()) {
        const auto message = "failed to open asu client config, path=" + configPath;
        return ASU_LOG_ERROR_STATUS(StatusCode::NOT_FOUND, message);
    }

    config = AsuClientConfig{};
    std::unordered_map<AsuId, AsuInfo> asuInfos;
    std::vector<std::pair<std::string, std::string>> transportFields;
    ClientConfigParseContext context{config, asuInfos};
    std::string line;
    while (std::getline(configFile, line)) {
        line = TrimConfigValue(line);
        if (line.empty() || line[0] == '#') { continue; }

        const auto pos = line.find('=');
        if (pos == std::string::npos) { continue; }

        const auto key = TrimConfigValue(line.substr(0, pos));
        const auto value = TrimConfigValue(line.substr(pos + 1));
        if (ApplyClientConfigField(context, key, value)) {
            continue;
        } else {
            AsuId asuId{0};
            std::string attrKey;
            if (TryParseAsuInfoKey(key, asuId)) {
                asuInfos[asuId] = ParseAsuInfo(value);
            } else if (TryGetTransportAttrKey(key, attrKey)) {
                transportFields.emplace_back(attrKey, value);
            }
        }
    }

    for (auto& transportConfig : config.transportConfigs) {
        for (const auto& field : transportFields) {
            if (ApplyTransportBufferConfigField(transportConfig, field.first, field.second)) {
                continue;
            }
            if (ApplyTransportIoNumConfigField(transportConfig, field.first, field.second)) {
                continue;
            }
            if (ApplyTransportProviderConfigField(transportConfig, field.first, field.second)) {
                continue;
            }
            transportConfig.attrs.emplace(field);
        }

        auto iter = asuInfos.find(transportConfig.asuId);
        if (iter == asuInfos.end()) { continue; }
        ApplyAsuInfoToTransportConfig(iter->second, transportConfig);
    }
    return Status::OK();
}

}  // namespace UC::ASU
