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
#include "config_parser_common.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace UC::ASU {
namespace {

using EndpointSetter = std::function<void(AsuEndpoint&, const std::string&)>;
using TransportConfigSetter = std::function<void(TransportConfig&, const std::string&)>;

void SetEndpointAttr(AsuEndpoint& endpoint, const std::string& key, const std::string& value)
{
    endpoint.attrs[key] = value;
}

// clang-format off
const std::unordered_map<std::string, EndpointSetter> g_transportEndpointSetters = {
    {"ip",                  [](AsuEndpoint& e, const std::string& v) { e.ip = v; }},
    {"local.comm_id",       [](AsuEndpoint& e, const std::string& v) { e.ip = v; }},
    {"localCommId",         [](AsuEndpoint& e, const std::string& v) { e.ip = v; }},
    {"port",                [](AsuEndpoint& e, const std::string& v) { e.port = static_cast<std::uint16_t>(ParseConfigUint64(v)); }},
    {"protocol",            [](AsuEndpoint& e, const std::string& v) { e.protocol = ParseConfigProtocol(v); }},
    {"numa_node",           [](AsuEndpoint& e, const std::string& v) { e.numaNode = static_cast<std::int32_t>(ParseConfigUint64(v)); }},
    {"numaNode",            [](AsuEndpoint& e, const std::string& v) { e.numaNode = static_cast<std::int32_t>(ParseConfigUint64(v)); }},
    {"device_id",           [](AsuEndpoint& e, const std::string& v) { e.deviceId = static_cast<std::int32_t>(ParseConfigUint64(v)); }},
    {"deviceId",            [](AsuEndpoint& e, const std::string& v) { e.deviceId = static_cast<std::int32_t>(ParseConfigUint64(v)); }},
    {"local.phy_device_id", [](AsuEndpoint& e, const std::string& v) { e.deviceId = static_cast<std::int32_t>(ParseConfigUint64(v)); }},
    {"localPhyDeviceId",    [](AsuEndpoint& e, const std::string& v) { e.deviceId = static_cast<std::int32_t>(ParseConfigUint64(v)); }},
    {"hca_name",            [](AsuEndpoint& e, const std::string& v) { e.hcaName = v; }},
    {"hcaName",             [](AsuEndpoint& e, const std::string& v) { e.hcaName = v; }},
    {"hca_port",            [](AsuEndpoint& e, const std::string& v) { e.hcaPort = static_cast<std::uint8_t>(ParseConfigUint64(v)); }},
    {"hcaPort",             [](AsuEndpoint& e, const std::string& v) { e.hcaPort = static_cast<std::uint8_t>(ParseConfigUint64(v)); }},
};

const std::unordered_map<std::string, EndpointSetter> g_clientViewEndpointSetters = {
    {"protocol",            [](AsuEndpoint& e, const std::string& v) { e.protocol = ParseConfigProtocol(v); SetEndpointAttr(e, "protocol", v); }},
    {"placement",           [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "placement", v); }},
    {"port",                [](AsuEndpoint& e, const std::string& v) { e.port = static_cast<std::uint16_t>(ParseConfigUint64(v)); }},
    {"local.comm_id",       [](AsuEndpoint& e, const std::string& v) { e.ip = v; }},
    {"localCommId",         [](AsuEndpoint& e, const std::string& v) { e.ip = v; }},
    {"local.phy_device_id", [](AsuEndpoint& e, const std::string& v) { e.deviceId = static_cast<std::int32_t>(ParseConfigUint64(v)); }},
    {"localPhyDeviceId",    [](AsuEndpoint& e, const std::string& v) { e.deviceId = static_cast<std::int32_t>(ParseConfigUint64(v)); }},
    {"tc",                  [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "tc", v); }},
    {"sl",                  [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "sl", v); }},
    {"send_size",           [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "send_size", v); }},
    {"sendSize",            [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "send_size", v); }},
    {"flag_size",           [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "flag_size", v); }},
    {"flagSize",            [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "flag_size", v); }},
    {"remote_send_addr",    [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "remote_send_addr", v); }},
    {"remoteSendAddr",      [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "remote_send_addr", v); }},
    {"remote_flag_addr",    [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "remote_flag_addr", v); }},
    {"remoteFlagAddr",      [](AsuEndpoint& e, const std::string& v) { SetEndpointAttr(e, "remote_flag_addr", v); }},
};

const std::unordered_map<std::string, TransportConfigSetter> g_transportBufferConfigSetters = {
    {"sendBufferSlotSize",              [](TransportConfig& c, const std::string& v) { c.sendBufferSlotSize = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"send_buffer_slot_size",           [](TransportConfig& c, const std::string& v) { c.sendBufferSlotSize = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"ioBuffer.sendBufferSlotSize",     [](TransportConfig& c, const std::string& v) { c.sendBufferSlotSize = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"io_buffer.send_buffer_slot_size", [](TransportConfig& c, const std::string& v) { c.sendBufferSlotSize = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"sendBufferSlotNum",               [](TransportConfig& c, const std::string& v) { c.sendBufferSlotNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"send_buffer_slot_num",            [](TransportConfig& c, const std::string& v) { c.sendBufferSlotNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"ioBuffer.sendBufferSlotNum",      [](TransportConfig& c, const std::string& v) { c.sendBufferSlotNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"io_buffer.send_buffer_slot_num",  [](TransportConfig& c, const std::string& v) { c.sendBufferSlotNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"flagBufferSlotSize",              [](TransportConfig& c, const std::string& v) { c.flagBufferSlotSize = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"flag_buffer_slot_size",           [](TransportConfig& c, const std::string& v) { c.flagBufferSlotSize = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"ioBuffer.flagBufferSlotSize",     [](TransportConfig& c, const std::string& v) { c.flagBufferSlotSize = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"io_buffer.flag_buffer_slot_size", [](TransportConfig& c, const std::string& v) { c.flagBufferSlotSize = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"flagBufferSlotNum",               [](TransportConfig& c, const std::string& v) { c.flagBufferSlotNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"flag_buffer_slot_num",            [](TransportConfig& c, const std::string& v) { c.flagBufferSlotNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"ioBuffer.flagBufferSlotNum",      [](TransportConfig& c, const std::string& v) { c.flagBufferSlotNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"io_buffer.flag_buffer_slot_num",  [](TransportConfig& c, const std::string& v) { c.flagBufferSlotNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
};

const std::unordered_map<std::string, TransportConfigSetter> g_transportIoNumConfigSetters = {
    {"batchLoadIoNum",    [](TransportConfig& c, const std::string& v) { c.asuBatchLoadIoNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"batch_load_io_num", [](TransportConfig& c, const std::string& v) { c.asuBatchLoadIoNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"batchStoreIoNum",   [](TransportConfig& c, const std::string& v) { c.asuBatchStoreIoNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"batch_store_io_num", [](TransportConfig& c, const std::string& v) { c.asuBatchStoreIoNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"deleteIoNum",       [](TransportConfig& c, const std::string& v) { c.asuDeleteIoNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"delete_io_num",     [](TransportConfig& c, const std::string& v) { c.asuDeleteIoNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"queryIoNum",        [](TransportConfig& c, const std::string& v) { c.asuQueryIoNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
    {"query_io_num",      [](TransportConfig& c, const std::string& v) { c.asuQueryIoNum = static_cast<std::size_t>(ParseConfigUint64(v)); }},
};

const std::unordered_map<std::string, TransportConfigSetter> g_transportProviderConfigSetters = {
    {"providerBackend",       [](TransportConfig& c, const std::string& v) { c.providerType = ParseConfigTransProviderType(v); }},
    {"provider_backend",      [](TransportConfig& c, const std::string& v) { c.providerType = ParseConfigTransProviderType(v); }},
    {"transProviderBackend",  [](TransportConfig& c, const std::string& v) { c.providerType = ParseConfigTransProviderType(v); }},
    {"trans_provider_backend", [](TransportConfig& c, const std::string& v) { c.providerType = ParseConfigTransProviderType(v); }},
};
// clang-format on

bool ApplyEndpointConfigField(const std::unordered_map<std::string, EndpointSetter>& setters,
                              AsuEndpoint& endpoint, const std::string& key,
                              const std::string& value)
{
    const auto iter = setters.find(key);
    if (iter == setters.end()) { return false; }
    iter->second(endpoint, value);
    return true;
}

}  // namespace

std::string TrimConfigValue(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) { return ""; }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> SplitConfigValue(const std::string& value, char delimiter)
{
    std::vector<std::string> parts;
    std::stringstream stream{value};
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        part = TrimConfigValue(part);
        if (!part.empty()) { parts.emplace_back(std::move(part)); }
    }
    return parts;
}

std::uint64_t ParseConfigUint64(const std::string& value) { return std::stoull(value, nullptr, 0); }

Protocol ParseConfigProtocol(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (value == "UB" || value == "UBOE") { return Protocol::UB; }
    if (value == "ROCE") { return Protocol::ROCE; }
    if (value == "TCP") { return Protocol::TCP; }
    return Protocol::TCP;
}

TransProviderType ParseConfigTransProviderType(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (value == "FAKE") { return TransProviderType::FAKE; }
    if (value == "AIV") { return TransProviderType::AIV; }
    if (value == "AICPU") { return TransProviderType::AICPU; }
    return TransProviderType::UNSUPPORTED;
}

bool ApplyTransportBufferConfigField(TransportConfig& config, const std::string& key,
                                     const std::string& value)
{
    const auto iter = g_transportBufferConfigSetters.find(key);
    if (iter == g_transportBufferConfigSetters.end()) { return false; }
    iter->second(config, value);
    return true;
}

bool ApplyTransportIoNumConfigField(TransportConfig& config, const std::string& key,
                                    const std::string& value)
{
    const auto iter = g_transportIoNumConfigSetters.find(key);
    if (iter == g_transportIoNumConfigSetters.end()) { return false; }
    iter->second(config, value);
    return true;
}

bool ApplyTransportProviderConfigField(TransportConfig& config, const std::string& key,
                                       const std::string& value)
{
    const auto iter = g_transportProviderConfigSetters.find(key);
    if (iter == g_transportProviderConfigSetters.end()) { return false; }
    iter->second(config, value);
    return true;
}

bool TryParseAsuInfoKey(const std::string& key, AsuId& asuId)
{
    constexpr const char* kCamelPrefix = "asuInfo.";
    constexpr const char* kSnakePrefix = "asu_info.";
    if (key.rfind(kCamelPrefix, 0) == 0) {
        asuId = std::stoull(key.substr(std::string{kCamelPrefix}.size()));
        return true;
    }
    if (key.rfind(kSnakePrefix, 0) == 0) {
        asuId = std::stoull(key.substr(std::string{kSnakePrefix}.size()));
        return true;
    }
    return false;
}

bool TryGetTransportAttrKey(const std::string& key, std::string& attrKey)
{
    constexpr const char* kCamelPrefix = "transport.";
    if (key.rfind(kCamelPrefix, 0) == 0) {
        attrKey = key.substr(std::string{kCamelPrefix}.size());
        return !attrKey.empty();
    }
    return false;
}

AsuEndpoint ParseTransportEndpoint(const std::string& value)
{
    AsuEndpoint endpoint;
    if (value.find('=') == std::string::npos) {
        auto parts = SplitConfigValue(value, ':');
        if (!parts.empty()) { endpoint.ip = parts[0]; }
        if (parts.size() > 1) {
            endpoint.port = static_cast<std::uint16_t>(ParseConfigUint64(parts[1]));
        }
        if (parts.size() > 2) { endpoint.protocol = ParseConfigProtocol(parts[2]); }
        return endpoint;
    }

    for (const auto& item : SplitConfigValue(value, ',')) {
        const auto pos = item.find('=');
        if (pos == std::string::npos) { continue; }
        const auto key = TrimConfigValue(item.substr(0, pos));
        const auto fieldValue = TrimConfigValue(item.substr(pos + 1));
        if (!ApplyEndpointConfigField(g_transportEndpointSetters, endpoint, key, fieldValue)) {
            endpoint.attrs[key] = fieldValue;
        }
    }
    return endpoint;
}

AsuEndpoint ParseClientViewEndpoint(const std::string& value)
{
    AsuEndpoint endpoint;
    if (value.find('=') == std::string::npos) {
        auto parts = SplitConfigValue(value, ':');
        if (!parts.empty()) { endpoint.ip = parts[0]; }
        if (parts.size() > 1) {
            endpoint.port = static_cast<std::uint16_t>(ParseConfigUint64(parts[1]));
        }
        if (parts.size() > 2) {
            endpoint.protocol = ParseConfigProtocol(parts[2]);
            SetEndpointAttr(endpoint, "protocol", parts[2]);
        }
        return endpoint;
    }

    for (const auto& item : SplitConfigValue(value, ',')) {
        const auto pos = item.find('=');
        if (pos == std::string::npos) { continue; }
        (void)ApplyEndpointConfigField(g_clientViewEndpointSetters, endpoint,
                                       TrimConfigValue(item.substr(0, pos)),
                                       TrimConfigValue(item.substr(pos + 1)));
    }
    return endpoint;
}

}  // namespace UC::ASU
