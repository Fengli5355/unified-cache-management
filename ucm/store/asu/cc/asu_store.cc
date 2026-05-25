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
#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include "asu_client_impl.h"
#include "asu_transport/asu_transport.h"
#include "logger/logger.h"
#include "ucmstore_v1.h"

namespace UC::AsuStore {
namespace {

using AsuStatus = UC::ASU::Status;
using AsuStatusCode = UC::ASU::StatusCode;

std::string ToHex(const Detail::BlockId& block)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (auto b : block) {
        os << std::setw(2) << static_cast<unsigned>(std::to_integer<unsigned char>(b));
    }
    return os.str();
}

Status ConvertStatus(const AsuStatus& status)
{
    if (status.ok()) { return Status::OK(); }

    const auto& message = status.message;
    switch (status.code) {
        case AsuStatusCode::INVALID_ARGUMENT: return Status::InvalidParam(message);
        case AsuStatusCode::NOT_FOUND:
        case AsuStatusCode::TASK_NOT_FOUND: return Status::NotFound();
        case AsuStatusCode::TIMEOUT: return Status::Timeout();
        case AsuStatusCode::BUFFER_NOT_SUPPORTED:
        case AsuStatusCode::UNSUPPORTED: return Status::Unsupported();
        case AsuStatusCode::RESOURCE_BUSY:
        case AsuStatusCode::IN_PROGRESS: return Status::Retry();
        default: return Status::Error(message);
    }
}

UC::ASU::MemoryType ParseMemoryType(const std::string& memoryType)
{
    if (memoryType == "host") { return UC::ASU::MemoryType::HOST; }
    if (memoryType == "host_pinned") { return UC::ASU::MemoryType::HOST_PINNED; }
    if (memoryType == "ascend_device") { return UC::ASU::MemoryType::ASCEND_DEVICE; }
    return UC::ASU::MemoryType::ASCEND_DEVICE;
}

std::string BuildKey(const Detail::BlockId& block, std::size_t shardIndex, std::size_t tensorIndex)
{
    return ToHex(block) + ":" + std::to_string(shardIndex) + ":" +
           std::to_string(tensorIndex);
}

}  // namespace

struct Config {
    std::string mode{"client"};
    std::string clientId{"ucm-asu-store"};
    std::vector<std::string> viewServiceAddrs;
    std::vector<ssize_t> asuIds;
    std::vector<std::string> asuIps;
    std::string asuNamePrefix{"asu"};
    std::uint16_t asuPort{0};
    std::uint64_t defaultWaitTimeoutMs{100};
    std::uint64_t queryTimeoutMs{5};
    std::uint64_t loadTimeoutMs{100};
    std::uint64_t storeTimeoutMs{100};
    std::uint64_t maxInflightTasks{1024};
    std::uint64_t maxInflightBytes{1ULL << 30};
    std::vector<std::size_t> tensorSizes;
    std::size_t shardSize{0};
    std::size_t blockSize{0};
    std::int32_t deviceId{-1};
    std::string memoryType;
};

class AsuBackend {
public:
    virtual ~AsuBackend() = default;
    virtual AsuStatus Init(const Config& config) = 0;
    virtual AsuStatus Shutdown() = 0;
    virtual AsuStatus Query(const std::vector<UC::ASU::CacheKey>& keys,
                            const UC::ASU::QueryOptions& options,
                            UC::ASU::QueryResult& result) = 0;
    virtual AsuStatus LoadAsync(const std::vector<UC::ASU::KVBuffer>& entries,
                                UC::ASU::TaskId& taskId) = 0;
    virtual AsuStatus StoreAsync(const std::vector<UC::ASU::KVBuffer>& entries,
                                 UC::ASU::TaskId& taskId) = 0;
    virtual AsuStatus Check(UC::ASU::TaskId taskId, UC::ASU::TaskResult& result) = 0;
    virtual AsuStatus Wait(UC::ASU::TaskId taskId, std::uint64_t timeoutMs,
                           UC::ASU::TaskResult& result) = 0;
};

UC::ASU::TransportConfig BuildTransportConfig(const Config& config, std::size_t index)
{
    UC::ASU::TransportConfig transportConfig;
    transportConfig.asuId = static_cast<UC::ASU::AsuId>(config.asuIds[index]);
    transportConfig.asuName = config.asuNamePrefix + "-" + std::to_string(config.asuIds[index]);
    transportConfig.queryTimeoutMs = config.queryTimeoutMs;
    transportConfig.loadTimeoutMs = config.loadTimeoutMs;
    transportConfig.storeTimeoutMs = config.storeTimeoutMs;
    transportConfig.maxInflightTasks = static_cast<std::uint32_t>(config.maxInflightTasks);
    transportConfig.maxInflightBytes = config.maxInflightBytes;
    if (!config.asuIps.empty()) {
        UC::ASU::AsuEndpoint endpoint;
        endpoint.ip = config.asuIps[index];
        endpoint.port = config.asuPort;
        endpoint.deviceId = config.deviceId;
        transportConfig.endpoints.emplace_back(std::move(endpoint));
    }
    return transportConfig;
}

class ClientBackend final : public AsuBackend {
public:
    AsuStatus Init(const Config& config) override
    {
        client_ = UC::ASU::CreateAsuClient();
        UC::ASU::AsuClientConfig asuConfig;
        asuConfig.clientId = config.clientId;
        asuConfig.viewServiceAddrs = config.viewServiceAddrs;
        asuConfig.defaultWaitTimeoutMs = config.defaultWaitTimeoutMs;
        asuConfig.transportConfigs.reserve(config.asuIds.size());
        for (std::size_t i = 0; i < config.asuIds.size(); ++i) {
            asuConfig.transportConfigs.emplace_back(BuildTransportConfig(config, i));
        }
        return client_->Init(asuConfig);
    }

    AsuStatus Shutdown() override
    {
        return client_ ? client_->Shutdown() : AsuStatus::OK();
    }

    AsuStatus Query(const std::vector<UC::ASU::CacheKey>& keys,
                    const UC::ASU::QueryOptions& options,
                    UC::ASU::QueryResult& result) override
    {
        return client_->Query(keys, options, result);
    }

    AsuStatus LoadAsync(const std::vector<UC::ASU::KVBuffer>& entries,
                        UC::ASU::TaskId& taskId) override
    {
        return client_->LoadAsync(entries, taskId);
    }

    AsuStatus StoreAsync(const std::vector<UC::ASU::KVBuffer>& entries,
                         UC::ASU::TaskId& taskId) override
    {
        return client_->StoreAsync(entries, taskId);
    }

    AsuStatus Check(UC::ASU::TaskId taskId, UC::ASU::TaskResult& result) override
    {
        return client_->Check(taskId, result);
    }

    AsuStatus Wait(UC::ASU::TaskId taskId, std::uint64_t timeoutMs,
                   UC::ASU::TaskResult& result) override
    {
        return client_->Wait(taskId, timeoutMs, result);
    }

private:
    std::unique_ptr<UC::ASU::AsuClient> client_;
};

class TransportBackend final : public AsuBackend {
public:
    AsuStatus Init(const Config& config) override
    {
        transport_ = UC::ASU::CreateAsuTransport();
        if (!transport_) {
            return AsuStatus::Error(AsuStatusCode::INTERNAL_ERROR,
                                    "ASU transport factory returned null");
        }
        return transport_->Init(BuildTransportConfig(config, 0));
    }

    AsuStatus Shutdown() override
    {
        return transport_ ? transport_->Shutdown() : AsuStatus::OK();
    }

    AsuStatus Query(const std::vector<UC::ASU::CacheKey>& keys,
                    const UC::ASU::QueryOptions& options,
                    UC::ASU::QueryResult& result) override
    {
        return transport_->Query(keys, options, result);
    }

    AsuStatus LoadAsync(const std::vector<UC::ASU::KVBuffer>& entries,
                        UC::ASU::TaskId& taskId) override
    {
        return transport_->LoadAsync(entries, taskId);
    }

    AsuStatus StoreAsync(const std::vector<UC::ASU::KVBuffer>& entries,
                         UC::ASU::TaskId& taskId) override
    {
        return transport_->StoreAsync(entries, taskId);
    }

    AsuStatus Check(UC::ASU::TaskId taskId, UC::ASU::TaskResult& result) override
    {
        return transport_->Check(taskId, result);
    }

    AsuStatus Wait(UC::ASU::TaskId taskId, std::uint64_t timeoutMs,
                   UC::ASU::TaskResult& result) override
    {
        return transport_->Wait(taskId, timeoutMs, result);
    }

private:
    std::unique_ptr<UC::ASU::AsuTransport> transport_;
};

class AsuStore final : public StoreV1 {
public:
    ~AsuStore() override
    {
        if (backend_) { (void)backend_->Shutdown(); }
    }

    Status Setup(const Detail::Dictionary& inConfig) override
    {
        auto config = ParseConfig(inConfig);
        auto status = CheckConfig(config);
        if (status.Failure()) { return status; }

        config_ = std::move(config);
        backend_ = CreateBackend(config_);

        auto asuStatus = backend_->Init(config_);
        if (!asuStatus.ok()) {
            UC_ERROR("Failed to init ASU backend: {}.", asuStatus.message);
            backend_.reset();
            return ConvertStatus(asuStatus);
        }

        ShowConfig(config_);
        return Status::OK();
    }

    std::string Readme() const override { return "AsuStore"; }

    Expected<std::vector<uint8_t>> Lookup(const Detail::BlockId* blocks, size_t num) override
    {
        std::vector<uint8_t> result(num, false);
        if (num == 0) { return result; }

        auto keys = BuildBlockKeys(blocks, num);
        UC::ASU::QueryResult queryResult;
        UC::ASU::QueryOptions options;
        options.timeoutMs = config_.queryTimeoutMs;
        auto status = backend_->Query(keys, options, queryResult);
        if (!status.ok()) { return ConvertStatus(status); }
        if (queryResult.exists.size() != keys.size()) {
            return Status::Error("ASU query result size mismatch");
        }

        const auto keysPerBlock = KeysPerBlock();
        for (std::size_t i = 0; i < num; ++i) {
            const auto begin = i * keysPerBlock;
            result[i] = std::all_of(queryResult.exists.begin() + begin,
                                    queryResult.exists.begin() + begin + keysPerBlock,
                                    [](auto value) { return value != 0; });
        }
        return result;
    }

    Expected<ssize_t> LookupOnPrefix(const Detail::BlockId* blocks, size_t num) override
    {
        auto lookup = Lookup(blocks, num);
        if (!lookup) { return lookup.Error(); }

        ssize_t prefix = -1;
        const auto& exists = lookup.Value();
        for (std::size_t i = 0; i < exists.size(); ++i) {
            if (exists[i] == 0) { break; }
            prefix = static_cast<ssize_t>(i);
        }
        return prefix;
    }

    void Prefetch(const Detail::BlockId* blocks, size_t num) override
    {
        (void)blocks;
        (void)num;
    }

    Expected<Detail::TaskHandle> Load(Detail::TaskDesc task) override
    {
        return Submit(std::move(task), &AsuBackend::LoadAsync);
    }

    Expected<Detail::TaskHandle> Dump(Detail::TaskDesc task) override
    {
        return Submit(std::move(task), &AsuBackend::StoreAsync);
    }

    Expected<bool> Check(Detail::TaskHandle taskId) override
    {
        UC::ASU::TaskResult result;
        auto status = backend_->Check(static_cast<UC::ASU::TaskId>(taskId), result);
        if (!status.ok()) { return ConvertStatus(status); }
        return result.status.code != AsuStatusCode::IN_PROGRESS;
    }

    Status Wait(Detail::TaskHandle taskId) override
    {
        UC::ASU::TaskResult result;
        auto status = backend_->Wait(static_cast<UC::ASU::TaskId>(taskId),
                                     config_.defaultWaitTimeoutMs, result);
        if (!status.ok()) { return ConvertStatus(status); }
        return ConvertStatus(result.status);
    }

private:
    using SubmitFunc = AsuStatus (AsuBackend::*)(const std::vector<UC::ASU::KVBuffer>&,
                                                 UC::ASU::TaskId&);

    Config ParseConfig(const Detail::Dictionary& inConfig)
    {
        Config config;
        inConfig.Get("asu_mode", config.mode);
        inConfig.Get("asu_client_id", config.clientId);
        inConfig.Get("asu_view_service_addrs", config.viewServiceAddrs);
        inConfig.GetNumbers("asu_ids", config.asuIds);
        inConfig.Get("asu_ips", config.asuIps);
        inConfig.Get("asu_name_prefix", config.asuNamePrefix);
        ssize_t asuPort = 0;
        inConfig.GetNumber("asu_port", asuPort);
        config.asuPort = static_cast<std::uint16_t>(std::max<ssize_t>(0, asuPort));
        inConfig.GetNumber("asu_default_wait_timeout_ms", config.defaultWaitTimeoutMs);
        inConfig.GetNumber("asu_query_timeout_ms", config.queryTimeoutMs);
        inConfig.GetNumber("asu_load_timeout_ms", config.loadTimeoutMs);
        inConfig.GetNumber("asu_store_timeout_ms", config.storeTimeoutMs);
        inConfig.GetNumber("asu_max_inflight_tasks", config.maxInflightTasks);
        inConfig.GetNumber("asu_max_inflight_bytes", config.maxInflightBytes);
        inConfig.GetNumber("shard_size", config.shardSize);
        inConfig.GetNumber("block_size", config.blockSize);
        inConfig.GetNumber("device_id", config.deviceId);
        inConfig.Get("asu_memory_type", config.memoryType);

        std::size_t tensorSize = 0;
        inConfig.GetNumber("tensor_size", tensorSize);
        if (tensorSize != 0) {
            if (config.shardSize != 0) { config.tensorSizes.assign(config.shardSize / tensorSize, tensorSize); }
        } else {
            inConfig.GetNumbers("tensor_size_list", config.tensorSizes);
        }
        return config;
    }

    Status CheckConfig(const Config& config)
    {
        if (config.mode != "client" && config.mode != "transport") {
            return Status::InvalidParam("invalid asu_mode({})", config.mode);
        }
        if (config.asuIds.empty()) { return Status::InvalidParam("invalid asu_ids"); }
        if (config.mode == "transport" && config.asuIds.size() != 1) {
            return Status::InvalidParam("transport mode requires exactly one asu_id");
        }
        if (!config.asuIps.empty() && config.asuIps.size() != config.asuIds.size()) {
            return Status::InvalidParam("asu_ips size must match asu_ids size");
        }
        if (config.tensorSizes.empty()) { return Status::InvalidParam("invalid tensor size"); }
        if (config.shardSize == 0) { return Status::InvalidParam("invalid shard size"); }
        if (config.blockSize == 0) { return Status::InvalidParam("invalid block size"); }
        const auto tensorSum = std::accumulate(config.tensorSizes.begin(), config.tensorSizes.end(),
                                               std::size_t{0});
        if (tensorSum != config.shardSize) {
            return Status::InvalidParam("invalid shard size({})", config.shardSize);
        }
        if (config.blockSize % config.shardSize != 0) {
            return Status::InvalidParam("invalid block size({})", config.blockSize);
        }
        return Status::OK();
    }

    std::unique_ptr<AsuBackend> CreateBackend(const Config& config)
    {
        if (config.mode == "transport") { return std::make_unique<TransportBackend>(); }
        return std::make_unique<ClientBackend>();
    }

    std::size_t ShardsPerBlock() const { return config_.blockSize / config_.shardSize; }

    std::size_t KeysPerBlock() const { return ShardsPerBlock() * config_.tensorSizes.size(); }

    std::vector<UC::ASU::CacheKey> BuildBlockKeys(const Detail::BlockId* blocks,
                                                  std::size_t num) const
    {
        std::vector<UC::ASU::CacheKey> keys;
        keys.reserve(num * KeysPerBlock());
        for (std::size_t blockIndex = 0; blockIndex < num; ++blockIndex) {
            for (std::size_t shardIndex = 0; shardIndex < ShardsPerBlock(); ++shardIndex) {
                for (std::size_t tensorIndex = 0; tensorIndex < config_.tensorSizes.size();
                     ++tensorIndex) {
                    keys.emplace_back(BuildKey(blocks[blockIndex], shardIndex, tensorIndex));
                }
            }
        }
        return keys;
    }

    Expected<Detail::TaskHandle> Submit(Detail::TaskDesc task, SubmitFunc submit)
    {
        auto entries = BuildKvBuffers(task);
        if (!entries) { return entries.Error(); }

        UC::ASU::TaskId taskId = UC::ASU::kInvalidTaskId;
        auto status = ((*backend_).*submit)(entries.Value(), taskId);
        if (!status.ok()) { return ConvertStatus(status); }
        return static_cast<Detail::TaskHandle>(taskId);
    }

    Expected<std::vector<UC::ASU::KVBuffer>> BuildKvBuffers(const Detail::TaskDesc& task) const
    {
        std::vector<UC::ASU::KVBuffer> entries;
        entries.reserve(task.size() * config_.tensorSizes.size());
        const auto memoryType = config_.memoryType.empty()
            ? (config_.deviceId >= 0 ? UC::ASU::MemoryType::ASCEND_DEVICE : UC::ASU::MemoryType::HOST)
            : ParseMemoryType(config_.memoryType);

        for (const auto& shard : task) {
            if (shard.index >= ShardsPerBlock()) {
                return Status::InvalidParam("invalid shard index({})", shard.index);
            }
            if (shard.addrs.size() != config_.tensorSizes.size()) {
                return Status::InvalidParam("invalid tensor addr count({})", shard.addrs.size());
            }
            for (std::size_t tensorIndex = 0; tensorIndex < shard.addrs.size(); ++tensorIndex) {
                UC::ASU::KVBuffer entry;
                entry.key = BuildKey(shard.owner, shard.index, tensorIndex);
                entry.buffer.region.memoryType = memoryType;
                entry.buffer.region.addr = reinterpret_cast<std::uint64_t>(shard.addrs[tensorIndex]);
                entry.buffer.region.size = config_.tensorSizes[tensorIndex];
                entry.buffer.region.deviceId = config_.deviceId;
                entry.buffer.handle = UC::ASU::kInvalidMRHandle;
                entries.emplace_back(std::move(entry));
            }
        }
        return entries;
    }

    void ShowConfig(const Config& config) const
    {
        UC_INFO("AsuStore.");
        UC_INFO("Set AsuStore::Mode to {}.", config.mode);
        UC_INFO("Set AsuStore::ClientId to {}.", config.clientId);
        UC_INFO("Set AsuStore::AsuIds to {}.", config.asuIds);
        UC_INFO("Set AsuStore::AsuIps to {}.", config.asuIps);
        UC_INFO("Set AsuStore::ShardSize to {}.", config.shardSize);
        UC_INFO("Set AsuStore::BlockSize to {}.", config.blockSize);
        UC_INFO("Set AsuStore::TensorSizes to {}.", config.tensorSizes);
        UC_INFO("Set AsuStore::DeviceId to {}.", config.deviceId);
    }

    Config config_;
    std::unique_ptr<AsuBackend> backend_;
};

}  // namespace UC::AsuStore

extern "C" UC::StoreV1* MakeAsuStore() { return new UC::AsuStore::AsuStore(); }
