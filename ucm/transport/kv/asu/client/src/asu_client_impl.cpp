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
#include "asu_client_impl.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>
#include "asu_transport/types.h"

namespace UC::ASU {

using RingNode = std::pair<std::uint64_t, AsuId>;
using RingData = std::vector<RingNode>;

constexpr AsuId kInvalidAsuId = std::numeric_limits<AsuId>::max();

std::uint64_t Crc32IEEE(const std::string& data)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
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

    std::uint32_t crc = 0xFFFFFFFFU;
    for (unsigned char ch : data) { crc = table[(crc ^ ch) & 0xFFU] ^ (crc >> 8U); }
    return crc ^ 0xFFFFFFFFU;
}

std::string BuildVirtualNodeKey(AsuId asuId, std::uint64_t index, std::uint64_t salt)
{
    auto key = "vn-" + std::to_string(index) + "#asu-" + std::to_string(asuId);
    if (salt == 0) { return key; }
    return key + "#" + std::to_string(salt);
}

bool HasHash(const RingData& ringData, std::uint64_t hashValue)
{
    return std::any_of(ringData.begin(), ringData.end(),
                       [hashValue](const RingNode& node) { return node.first == hashValue; });
}

void InsertAsuVirtualNode(RingData& ringData, AsuId asuId, std::uint64_t index,
                          const HashFunction& hash)
{
    std::uint64_t salt = 0;
    auto hashValue = hash(BuildVirtualNodeKey(asuId, index, salt));
    while (HasHash(ringData, hashValue)) {
        ++salt;
        hashValue = hash(BuildVirtualNodeKey(asuId, index, salt));
    }
    ringData.emplace_back(hashValue, asuId);
}

Status PartialFailed(const std::string& message)
{
    return Status::Error(StatusCode::PARTIAL_FAILED, message);
}

bool IsPrime(std::uint64_t value)
{
    if (value < 2) { return false; }
    if (value == 2) { return true; }
    if (value % 2 == 0) { return false; }
    for (std::uint64_t factor = 3; factor <= value / factor; factor += 2) {
        if (value % factor == 0) { return false; }
    }
    return true;
}

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) { return ""; }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> Split(const std::string& value, char delimiter)
{
    std::vector<std::string> parts;
    std::stringstream stream{value};
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        part = Trim(part);
        if (!part.empty()) { parts.emplace_back(std::move(part)); }
    }
    return parts;
}

class ConfigFileViewServer final : public ViewServer {
public:
    explicit ConfigFileViewServer(std::string configPath) : configPath_(std::move(configPath)) {}

    Status GetGlobalView(GlobalView& view) override
    {
        std::ifstream configFile{configPath_};
        if (!configFile.is_open()) {
            return Status::Error(StatusCode::NOT_FOUND,
                                 "failed to open global view config, path=" + configPath_);
        }

        GlobalView nextView;
        std::string line;
        while (std::getline(configFile, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == '#') { continue; }

            const auto pos = line.find('=');
            if (pos == std::string::npos) { continue; }

            const auto key = Trim(line.substr(0, pos));
            const auto value = Trim(line.substr(pos + 1));
            if (key == "viewEpoch" || key == "view_epoch") {
                nextView.viewEpoch = std::stoull(value);
            } else if (key == "viewId" || key == "view_id") {
                nextView.viewId = std::stoull(value);
            } else if (key == "createTimeMs" || key == "create_time_ms") {
                nextView.createTimeMs = std::stoull(value);
            } else if (key == "expireTimeMs" || key == "expire_time_ms") {
                nextView.expireTimeMs = std::stoull(value);
            } else if (key == "asuIds" || key == "asu_ids") {
                nextView.asuMap.clear();
                for (const auto& asuId : Split(value, ',')) {
                    AsuInfo asuInfo;
                    asuInfo.asuId = std::stoull(asuId);
                    nextView.asuMap.emplace(asuInfo.asuId, asuInfo);
                }
            }
        }

        view = std::move(nextView);
        return Status::OK();
    }

private:
    std::string configPath_;
};

AsuClientImpl::Router::Router(const std::vector<AsuId>& asuIds, HashFunction hash,
                              HashTableConfig config)
    : hash_(std::move(hash)), hashTable_(config)
{
    if (!hash_) { hash_ = Crc32IEEE; }
    if (hashTable_.type == HashTableType::MAGLEV) {
        BuildMaglev(asuIds);
    } else {
        BuildRing(asuIds);
    }
}

void AsuClientImpl::Router::BuildRing(const std::vector<AsuId>& asuIds)
{
    RingData ringData;
    std::vector<AsuId> addedAsuIds;
    for (auto asuId : asuIds) {
        if (asuId == kInvalidAsuId ||
            std::find(addedAsuIds.begin(), addedAsuIds.end(), asuId) != addedAsuIds.end()) {
            continue;
        }

        addedAsuIds.emplace_back(asuId);
        for (std::uint64_t index = 0; index < hashTable_.virtualNodeCount; ++index) {
            InsertAsuVirtualNode(ringData, asuId, index, hash_);
        }
    }

    std::sort(ringData.begin(), ringData.end());
    ring_ = std::move(ringData);
}

void AsuClientImpl::Router::BuildMaglev(const std::vector<AsuId>& asuIds)
{
    if (!IsPrime(hashTable_.maglevTableSize)) {
        hashTable_.maglevTableSize = kDefaultMaglevTableSize;
    }

    std::vector<AsuId> activeAsuIds;
    for (auto asuId : asuIds) {
        if (asuId == kInvalidAsuId ||
            std::find(activeAsuIds.begin(), activeAsuIds.end(), asuId) != activeAsuIds.end()) {
            continue;
        }
        activeAsuIds.emplace_back(asuId);
    }
    if (activeAsuIds.empty()) { return; }

    lookupTable_.assign(hashTable_.maglevTableSize, kInvalidAsuId);
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> skips;
    std::vector<std::uint64_t> next;
    offsets.reserve(activeAsuIds.size());
    skips.reserve(activeAsuIds.size());
    next.assign(activeAsuIds.size(), 0);

    for (auto asuId : activeAsuIds) {
        auto value = std::to_string(asuId);
        offsets.emplace_back(hash_("maglev-offset#asu-" + value) % hashTable_.maglevTableSize);
        skips.emplace_back(hash_("maglev-skip#asu-" + value) % (hashTable_.maglevTableSize - 1) +
                           1);
    }

    std::uint64_t filled = 0;
    while (filled < hashTable_.maglevTableSize) {
        for (std::size_t index = 0;
             index < activeAsuIds.size() && filled < hashTable_.maglevTableSize; ++index) {
            auto candidate =
                (offsets[index] + next[index] * skips[index]) % hashTable_.maglevTableSize;
            ++next[index];
            while (lookupTable_[candidate] != kInvalidAsuId) {
                candidate =
                    (offsets[index] + next[index] * skips[index]) % hashTable_.maglevTableSize;
                ++next[index];
            }
            lookupTable_[candidate] = activeAsuIds[index];
            ++filled;
        }
    }
}

std::unordered_map<AsuId, std::vector<AsuClientImpl::EntryIndex>> AsuClientImpl::Router::RouteKeys(
    const std::vector<CacheKey>& keys) const
{
    std::unordered_map<AsuId, std::vector<EntryIndex>> routes;
    for (EntryIndex index = 0; index < keys.size(); ++index) {
        auto asuId = RouteKey(keys[index]);
        if (asuId != kInvalidAsuId) { routes[asuId].emplace_back(index); }
    }
    return routes;
}

std::unordered_map<AsuId, std::vector<AsuClientImpl::EntryIndex>>
AsuClientImpl::Router::RouteEntries(const std::vector<KVBuffer>& entries) const
{
    std::unordered_map<AsuId, std::vector<EntryIndex>> routes;
    for (EntryIndex index = 0; index < entries.size(); ++index) {
        auto asuId = RouteKey(entries[index].key);
        if (asuId != kInvalidAsuId) { routes[asuId].emplace_back(index); }
    }
    return routes;
}

AsuId AsuClientImpl::Router::RouteKey(const CacheKey& key) const
{
    if (!lookupTable_.empty()) { return lookupTable_[hash_(key) % lookupTable_.size()]; }
    if (ring_.empty()) { return kInvalidAsuId; }

    const auto hashValue = hash_(key);
    auto iter = std::lower_bound(
        ring_.begin(), ring_.end(), hashValue,
        [](const RingNode& ringNode, std::uint64_t value) { return ringNode.first < value; });
    if (iter == ring_.end()) { iter = ring_.begin(); }
    return iter->second;
}

AsuClientImpl::AsuClientImpl(TransportFactory factory) : transportFactory_(std::move(factory))
{
    if (!transportFactory_) { transportFactory_ = CreateAsuTransport; }
}

AsuClientImpl::~AsuClientImpl() { Shutdown(); }

AsuId AsuClientImpl::Router::Pick(const CacheKey& key) const
{
    if (asu_ids.empty()) { return 0; }
    const auto index = std::hash<CacheKey>{}(key) % asu_ids.size();
    return asu_ids[index];
}

Status AsuClientImpl::Init(const AsuClientConfig& config)
{
    if (view_) { return Status::OK(); }

    auto view = std::make_shared<ViewSnapshot>();
    view->router = std::make_shared<Router>();
    AsuId next_generated_asu_id = 1;
    for (const auto& transport_config : config.transport_configs) {
        auto asu_id = transport_config.asu_id;
        if (asu_id == 0) {
            while (view->transports.find(next_generated_asu_id) != view->transports.end()) {
                ++next_generated_asu_id;
            }
            asu_id = next_generated_asu_id++;
        } else if (view->transports.find(asu_id) != view->transports.end()) {
            return Status::Error(StatusCode::INVALID_ARGUMENT, "duplicate ASU transport id");
        }

        auto transport = transport_factory_();
        if (!transport) {
            return Status::Error(StatusCode::INTERNAL_ERROR, "transport factory returned null");
        }

        auto status = transport->Init(transport_config);
        if (!status.ok()) { return status; }

        view->router->asu_ids.push_back(asu_id);
        view->transports.emplace(asu_id, std::shared_ptr<AsuTransport>(std::move(transport)));
    }
    config_ = config;
    view_ = std::move(view);
    return Status::OK();
}

template <typename Operation>
Status AsuClientImpl::RunWithRefreshRetry(const std::string& operationName, Operation operation)
{
    Status status = Status::OK();
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
        bool needRefresh = false;
        status = operation(needRefresh);
        if (!needRefresh || attempt != 0) { return status; }

        auto refreshStatus = RefreshView();
        if (!refreshStatus.ok()) {
            return WithContext(refreshStatus,
                               "failed to refresh view before retrying " + operationName);
        }
    }
    return status;
}

Status AsuClientImpl::Init(const AsuClientConfig& config)
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (initialized_) {
            return Status::Error(StatusCode::RESOURCE_BUSY,
                                 "asu client has already been initialized");
        }
    }

    {
        std::lock_guard<std::mutex> lock{mutex_};
        config_ = config;
        viewServer_ = config.viewServer;
        if (viewServer_ == nullptr && !config.viewServiceAddrs.empty()) {
            viewServer_ = std::make_shared<ConfigFileViewServer>(config.viewServiceAddrs.front());
        }
        transportConfigs_.clear();
        for (const auto& transportConfig : config.transportConfigs) {
            transportConfigs_[transportConfig.asu_id] = transportConfig;
        }
    }

    GlobalView view;
    auto status = FetchGlobalView(view);
    if (!status.ok()) { return status; }

    std::shared_ptr<ViewSnapshot> nextSnapshot;
    status = BuildSnapshot(config, view, nullptr, nextSnapshot);
    if (!status.ok()) { return status; }

    std::lock_guard<std::mutex> lock{mutex_};
    if (initialized_) {
        for (auto& item : nextSnapshot->transports) { item.second->Shutdown(); }
        return Status::Error(StatusCode::RESOURCE_BUSY, "asu client has already been initialized");
    }
    snapshot_ = std::move(nextSnapshot);
    initialized_ = true;
    return Status::OK();
}

Status AsuClientImpl::Shutdown()
{
    std::shared_ptr<ViewSnapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        snapshot = std::move(snapshot_);
        tasks_.clear();
        config_ = AsuClientConfig{};
        viewServer_.reset();
        transportConfigs_.clear();
        registeredResources_.clear();
        initialized_ = false;
    }

    Status finalStatus = Status::OK();
    if (snapshot) {
        for (auto& item : snapshot->transports) {
            auto status = item.second->Shutdown();
            if (!status.ok() && finalStatus.ok()) { finalStatus = status; }
        }
    }
    return finalStatus;
}

Status AsuClientImpl::Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                            QueryResult& result)
{
    auto view = view_;
    if (!view || !view->router || view->transports.empty()) {
        return Status::Error(StatusCode::NOT_INITIALIZED, "client has no ASU transports");
    }

    result.exists.assign(keys.size(), 0);
    result.prefix_hit_keys = 0;

    return Status::OK();
}

Status AsuClientImpl::LoadAsync(const std::vector<KVBuffer>& entries, TaskId& task_id)
{
    return SubmitAsync(ClientOpType::LOAD, entries, task_id);
}

Status AsuClientImpl::StoreAsync(const std::vector<KVBuffer>& entries, TaskId& task_id)
{
    return SubmitAsync(ClientOpType::STORE, entries, task_id);
}

Status AsuClientImpl::DeleteAsync(const std::vector<CacheKey>& keys, TaskId& task_id)
{
    (void)keys;
    task_id = kInvalidTaskId;
    return Status::Error(StatusCode::UNSUPPORTED, "client delete async is not supported now");
}

Status AsuClientImpl::Check(TaskId task_id, TaskResult& result)
{
    auto ctx = task_manager_.Get(task_id);
    if (!ctx) { return Status::Error(StatusCode::TASK_NOT_FOUND, "client task not found"); }

    PollTask(ctx);
    return BuildResult(ctx, result);
}

Status AsuClientImpl::Wait(TaskId task_id, std::uint64_t timeout_ms, TaskResult& result)
{
    auto ctx = task_manager_.Get(task_id);
    if (!ctx) { return Status::Error(StatusCode::TASK_NOT_FOUND, "client task not found"); }

    const auto wait_ms = timeout_ms == 0 ? config_.default_wait_timeout_ms : timeout_ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

    while (!ctx->Done()) {
        if (PollTask(ctx)) { break; }
        if (wait_ms != 0 && std::chrono::steady_clock::now() >= deadline) {
            BuildResult(ctx, result);
            result.status = Status::Error(StatusCode::TIMEOUT, "client task wait timeout");
            return result.status;
        }
        // TODO: maybe no busy wait
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    auto status = BuildResult(ctx, result);
    if (!status.ok()) { return status; }
    task_manager_.Remove(task_id);
    return Status::OK();
}

Status AsuClientImpl::RegisterRegions(const std::vector<MemoryRegion>& regions,
                                      std::vector<RegisterResult>& results)
{
    auto view = view_;
    if (!view || view->transports.empty()) {
        return Status::Error(StatusCode::NOT_INITIALIZED, "client has no ASU transports");
    }

    results.assign(regions.size(), RegisterResult{Status::OK(), kInvalidMRHandle});
    // TODO: register or bind
    return Status::OK();
}
Status AsuClientImpl::Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                            QueryResult& result)
{
    return RunWithRefreshRetry(
        "query", [&](bool& needRefresh) { return QueryOnce(keys, options, result, needRefresh); });
}

Status AsuClientImpl::QueryOnce(const std::vector<CacheKey>& keys, const QueryOptions& options,
                                QueryResult& result, bool& needRefresh)
{
    result.exists.assign(keys.size(), 0);
    result.prefix_hit_keys = 0;

    auto snapshot = GetSnapshot();
    if (!snapshot) { return NotInitialized(); }

    if (options.mode == QueryMode::PREFIX) {
        Status finalStatus = Status::OK();
        for (const auto& item : snapshot->transports) {
            QueryResult childResult;
            auto status = item.second->Query(keys, options, childResult);
            if (!status.ok()) {
                MarkRefreshIfNeeded(status, needRefresh);
                finalStatus = WithContext(PartialFailed("one or more asu prefix queries failed"),
                                          "asuId=" + std::to_string(item.first));
                continue;
            }
            result.prefix_hit_keys += childResult.prefix_hit_keys;
            for (std::size_t index = 0;
                 index < result.exists.size() && index < childResult.exists.size(); ++index) {
                result.exists[index] = result.exists[index] || childResult.exists[index];
            }
        }
        return finalStatus;
    }

    auto routes = snapshot->router->RouteKeys(keys);
    for (const auto& route : routes) {
        auto transportIter = snapshot->transports.find(route.first);
        if (transportIter == snapshot->transports.end()) {
            auto status = Status::Error(StatusCode::NOT_FOUND, "routed asu transport not found");
            MarkRefreshIfNeeded(status, needRefresh);
            return WithContext(status, "asuId=" + std::to_string(route.first));
        }

        std::vector<CacheKey> childKeys;
        childKeys.reserve(route.second.size());
        for (auto index : route.second) { childKeys.emplace_back(keys[index]); }

        QueryResult childResult;
        auto status = transportIter->second->Query(childKeys, options, childResult);
        if (!status.ok()) {
            MarkRefreshIfNeeded(status, needRefresh);
            return WithContext(status, "asuId=" + std::to_string(route.first) +
                                           " key_count=" + std::to_string(childKeys.size()));
        }
        if (childResult.exists.size() != childKeys.size()) {
            return Status::Error(
                StatusCode::INTERNAL_ERROR,
                "query result size mismatch, asuId=" + std::to_string(route.first) +
                    " expected=" + std::to_string(childKeys.size()) +
                    " actual=" + std::to_string(childResult.exists.size()));
        }

        for (std::size_t index = 0; index < route.second.size(); ++index) {
            result.exists[route.second[index]] = childResult.exists[index];
        }
        result.prefix_hit_keys += childResult.prefix_hit_keys;
    }

    return Status::OK();
}

Status AsuClientImpl::LoadAsync(const std::vector<KVBuffer>& entries, TaskId& taskId)
{
    return SubmitEntries(entries, taskId, &AsuTransport::LoadAsync);
}

Status AsuClientImpl::StoreAsync(const std::vector<KVBuffer>& entries, TaskId& taskId)
{
    return SubmitEntries(entries, taskId, &AsuTransport::StoreAsync);
}

Status AsuClientImpl::DeleteAsync(const std::vector<CacheKey>& keys, TaskId& taskId)
{
    return SubmitDelete(keys, taskId);
}

Status AsuClientImpl::Check(TaskId taskId, TaskResult& result)
{
    AggregateTask task;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto iter = tasks_.find(taskId);
        if (iter == tasks_.end()) {
            return Status::Error(StatusCode::TASK_NOT_FOUND, "task not found");
        }
        task = iter->second;
    }
    bool needRefresh = false;
    auto status = CollectTaskResult(task, false, 0, result, needRefresh);
    if (IsTaskComplete(result)) { RemoveAggregateTask(taskId); }
    if (needRefresh) {
        auto refreshStatus = RefreshView();
        if (!refreshStatus.ok()) {
            return WithContext(refreshStatus, "failed to refresh view after checking taskId=" +
                                                  std::to_string(taskId));
        }
    }
    return status;
}

Status AsuClientImpl::Wait(TaskId taskId, std::uint64_t timeoutMs, TaskResult& result)
{
    AggregateTask task;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        auto iter = tasks_.find(taskId);
        if (iter == tasks_.end()) {
            return Status::Error(StatusCode::TASK_NOT_FOUND, "task not found");
        }
        task = iter->second;
    }
    bool needRefresh = false;
    auto status = CollectTaskResult(task, true, timeoutMs, result, needRefresh);
    if (IsTaskComplete(result)) { RemoveAggregateTask(taskId); }
    if (needRefresh) {
        auto refreshStatus = RefreshView();
        if (!refreshStatus.ok()) {
            return WithContext(refreshStatus, "failed to refresh view after waiting taskId=" +
                                                  std::to_string(taskId));
        }
    }
    return status;
}

Status AsuClientImpl::RegisterRegions(const std::vector<MemoryRegion>& regions,
                                      std::vector<RegisterResult>& results)
{
    return RunWithRefreshRetry("register regions", [&](bool& needRefresh) {
        return RegisterRegionsOnce(regions, results, needRefresh);
    });
}

Status AsuClientImpl::RegisterRegionsOnce(const std::vector<MemoryRegion>& regions,
                                          std::vector<RegisterResult>& results, bool& needRefresh)
{
    auto snapshot = GetSnapshot();
    if (!snapshot) { return NotInitialized(); }

    results.clear();
    if (snapshot->transports.empty()) { return Status::OK(); }

    auto firstIter = snapshot->transports.find(snapshot->asuIds.front());
    if (firstIter == snapshot->transports.end()) {
        auto status = Status::Error(StatusCode::NOT_FOUND, "first asu transport not found");
        MarkRefreshIfNeeded(status, needRefresh);
        return WithContext(status, "asuIndex=0 asuId=" + std::to_string(snapshot->asuIds.front()));
    }

    auto status = firstIter->second->RegisterRegions(regions, results);
    if (!status.ok()) {
        MarkRefreshIfNeeded(status, needRefresh);
        return WithContext(status, "asuIndex=0 asuId=" + std::to_string(snapshot->asuIds.front()) +
                                       " region_count=" + std::to_string(regions.size()));
    }

    std::vector<RegisteredMemory> registeredRegions;
    registeredRegions.reserve(regions.size());
    for (std::size_t index = 0; index < regions.size(); ++index) {
        RegisteredMemory registeredRegion;
        registeredRegion.region = regions[index];
        if (index < results.size()) { registeredRegion.handle = results[index].handle; }
        registeredRegions.emplace_back(registeredRegion);
    }

    Status finalStatus = Status::OK();
    for (std::size_t asuIndex = 1; asuIndex < snapshot->asuIds.size(); ++asuIndex) {
        auto iter = snapshot->transports.find(snapshot->asuIds[asuIndex]);
        if (iter == snapshot->transports.end()) {
            auto status = Status::Error(StatusCode::NOT_FOUND, "bound asu transport not found");
            MarkRefreshIfNeeded(status, needRefresh);
            finalStatus = WithContext(PartialFailed("one or more asu region bindings failed"),
                                      "asuIndex=" + std::to_string(asuIndex) +
                                          " asuId=" + std::to_string(snapshot->asuIds[asuIndex]));
            continue;
        }

        std::vector<RegisterResult> childResults;
        status = iter->second->BindRegisteredRegions(registeredRegions, childResults);
        if (!status.ok() && finalStatus.ok()) {
            MarkRefreshIfNeeded(status, needRefresh);
            finalStatus =
                WithContext(PartialFailed("one or more asu region bindings failed"),
                            "asuIndex=" + std::to_string(asuIndex) +
                                " asuId=" + std::to_string(snapshot->asuIds[asuIndex]) +
                                " region_count=" + std::to_string(registeredRegions.size()));
        }
    }

    if (finalStatus.ok()) {
        std::lock_guard<std::mutex> lock{mutex_};
        for (std::size_t index = 0; index < regions.size() && index < results.size(); ++index) {
            registeredResources_.emplace_back(RegisteredResource{regions[index], results[index]});
        }
    }
    return finalStatus;
}

Status AsuClientImpl::SubmitAsync(ClientOpType op_type, const std::vector<KVBuffer>& entries,
                                  TaskId& task_id)
{
    auto view = view_;
    if (!view || !view->router || view->transports.empty()) {
        task_id = kInvalidTaskId;
        return Status::Error(StatusCode::NOT_INITIALIZED, "client has no ASU transports");
    }

    auto ctx = std::make_unique<ClientTaskContext>();
    ctx->op_type = op_type;
    const auto count = entries.size();
    ctx->entry_status.assign(count, Status::OK());

    std::unordered_map<AsuId, std::size_t> group_index;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& key = entries[i].key;
        const auto asu_id = view->router->Pick(key);
        auto iter = group_index.find(asu_id);
        if (iter == group_index.end()) {
            iter = group_index.emplace(asu_id, ctx->sub_tasks.size()).first;
            ClientSubTask sub_task;
            sub_task.asu_id = asu_id;
            ctx->sub_tasks.push_back(std::move(sub_task));
        }
        auto& sub_task = ctx->sub_tasks[iter->second];
        sub_task.entries.push_back(entries[i]);
        sub_task.original_indices.push_back(i);
    }

    auto status = task_manager_.Submit(std::move(ctx), task_id);
    if (!status.ok()) { return status; }

    auto raw_ctx = task_manager_.Get(task_id);
    if (!raw_ctx) {
        task_id = kInvalidTaskId;
        return Status::Error(StatusCode::INTERNAL_ERROR, "client task disappeared after submit");
    }

    status = DispatchTask(raw_ctx);
    if (!status.ok()) {
        task_manager_.Remove(task_id);
        task_id = kInvalidTaskId;
        return status;
    }

    raw_ctx->state.store(ClientTaskState::INFLIGHT, std::memory_order_release);
    return Status::OK();
}

Status AsuClientImpl::DispatchTask(const ClientTaskContextPtr& ctx)
{
    auto view = view_;
    if (!view) { return Status::Error(StatusCode::NOT_INITIALIZED, "client view is not ready"); }

    for (auto& sub_task : ctx->sub_tasks) {
        auto trans_iter = view->transports.find(sub_task.asu_id);
        if (trans_iter == view->transports.end()) {
            return Status::Error(StatusCode::NOT_FOUND, "routed ASU transport not found");
        }

        Status status;
        if (ctx->op_type == ClientOpType::LOAD) {
            status = trans_iter->second->LoadAsync(sub_task.entries, sub_task.trans_task_id);
        } else if (ctx->op_type == ClientOpType::STORE) {
            status = trans_iter->second->StoreAsync(sub_task.entries, sub_task.trans_task_id);
        } else {
            status = trans_iter->second->DeleteAsync(sub_task.keys, sub_task.trans_task_id);
        }
        // TODO: deal with partial dispatch failure
        if (!status.ok()) { return status; }
    }
    return Status::OK();
}

bool AsuClientImpl::PollTask(const ClientTaskContextPtr& ctx)
{
    auto view = view_;
    if (!ctx || ctx->Done()) { return true; }
    if (!view || ctx->state.load(std::memory_order_acquire) != ClientTaskState::INFLIGHT) {
        return false;
    }

    bool all_done = true;
    bool any_failed = false;
    for (auto& sub_task : ctx->sub_tasks) {
        auto trans_iter = view->transports.find(sub_task.asu_id);
        if (trans_iter == view->transports.end()) {
            any_failed = true;
            continue;
        }

        TaskResult sub_result;
        auto status = trans_iter->second->Check(sub_task.trans_task_id, sub_result);
        if (!status.ok()) {
            any_failed = true;
            continue;
        }
        if (sub_result.status.code == StatusCode::IN_PROGRESS) {
            all_done = false;
            continue;
        }
        if (!sub_result.status.ok()) { any_failed = true; }

        const auto& original_indices = sub_task.original_indices;
        for (std::size_t i = 0; i < original_indices.size() && i < sub_result.entry_status.size();
             ++i) {
            ctx->entry_status[original_indices[i]] = sub_result.entry_status[i];
        }
    }

    if (all_done) {
        ctx->final_status =
            any_failed ? Status::Error(StatusCode::PARTIAL_FAILED, "client task partially failed")
                       : Status::OK();
        ctx->state.store(any_failed ? ClientTaskState::FAILED : ClientTaskState::COMPLETED,
                         std::memory_order_release);
        return true;
    }
    return false;
}

Status AsuClientImpl::BuildResult(const ClientTaskContextPtr& ctx, TaskResult& result)
{
    result.status = ctx->Done() ? ctx->final_status
                                : Status::Error(StatusCode::IN_PROGRESS, "client task in progress");
    result.entry_status = ctx->entry_status;
    result.query_result.reset();
    return Status::OK();
}
Status AsuClientImpl::UnregisterRegions(const std::vector<MRHandle>& handles)
{
    return RunWithRefreshRetry("unregister regions", [&](bool& needRefresh) {
        return UnregisterRegionsOnce(handles, needRefresh);
    });
}

Status AsuClientImpl::UnregisterRegionsOnce(const std::vector<MRHandle>& handles, bool& needRefresh)
{
    auto snapshot = GetSnapshot();
    if (!snapshot) { return NotInitialized(); }

    Status finalStatus = Status::OK();
    for (const auto& item : snapshot->transports) {
        auto status = item.second->UnregisterRegions(handles);
        if (!status.ok() && finalStatus.ok()) {
            MarkRefreshIfNeeded(status, needRefresh);
            finalStatus =
                WithContext(status, "asuId=" + std::to_string(item.first) +
                                        " handle_count=" + std::to_string(handles.size()));
        }
    }
    if (finalStatus.ok()) {
        std::lock_guard<std::mutex> lock{mutex_};
        registeredResources_.erase(
            std::remove_if(registeredResources_.begin(), registeredResources_.end(),
                           [&handles](const RegisteredResource& resource) {
                               return std::find(handles.begin(), handles.end(),
                                                resource.result.handle) != handles.end();
                           }),
            registeredResources_.end());
    }
    return finalStatus;
}

Status AsuClientImpl::FetchGlobalView(GlobalView& view)
{
    std::shared_ptr<ViewServer> viewServer;
    AsuClientConfig config;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        config = config_;
        viewServer = viewServer_;
    }

    if (viewServer != nullptr) { return viewServer->GetGlobalView(view); }

    view = MakeConfigGlobalView(config);
    return Status::OK();
}

bool AsuClientImpl::IsStaleViewLocked(const GlobalView& view) const
{
    if (!HasKnownViewEpoch(view) || snapshot_ == nullptr || !HasKnownViewEpoch(snapshot_->view)) {
        return false;
    }
    return view.viewEpoch <= snapshot_->view.viewEpoch;
}

Status AsuClientImpl::BuildSnapshot(const AsuClientConfig& config, const GlobalView& view,
                                    const std::shared_ptr<ViewSnapshot>& oldSnapshot,
                                    std::shared_ptr<ViewSnapshot>& snapshot)
{
    auto nextSnapshot = std::make_shared<ViewSnapshot>();
    auto asuIds = GetSortedAsuIds(view);
    nextSnapshot->view = view;

    for (std::size_t asuIndex = 0; asuIndex < asuIds.size(); ++asuIndex) {
        auto asuId = asuIds[asuIndex];
        std::shared_ptr<AsuTransport> transport;
        if (oldSnapshot != nullptr) {
            auto oldIter = oldSnapshot->transports.find(asuId);
            if (oldIter != oldSnapshot->transports.end()) { transport = oldIter->second; }
        }

        if (transport == nullptr) {
            auto status = BuildTransport(asuId, transport);
            if (!status.ok()) {
                return WithContext(status, "asuIndex=" + std::to_string(asuIndex) +
                                               " asuId=" + std::to_string(asuId));
            }

            status = BindRegisteredResources(asuId, transport);
            if (!status.ok()) {
                transport->Shutdown();
                return WithContext(
                    status, "bind registered resources during view refresh, asuIndex=" +
                                std::to_string(asuIndex) + " asuId=" + std::to_string(asuId));
            }
        }

        nextSnapshot->transports.emplace(asuId, std::move(transport));
    }

    nextSnapshot->router = std::make_shared<Router>(asuIds, config.hash, config.hashTable);
    nextSnapshot->asuIds = std::move(asuIds);
    snapshot = std::move(nextSnapshot);
    return Status::OK();
}

Status AsuClientImpl::BuildTransport(AsuId asuId, std::shared_ptr<AsuTransport>& transport)
{
    auto configIter = transportConfigs_.find(asuId);
    if (configIter == transportConfigs_.end()) {
        return Status::Error(StatusCode::NOT_FOUND,
                             "transport config not found, asuId=" + std::to_string(asuId));
    }

    auto nextTransport = transportFactory_();
    if (!nextTransport) {
        return Status::Error(StatusCode::INTERNAL_ERROR,
                             "transport factory returned null, asuId=" + std::to_string(asuId));
    }

    auto status = nextTransport->Init(configIter->second);
    if (!status.ok()) {
        return WithContext(status, "init transport failed, asuId=" + std::to_string(asuId));
    }

    transport = std::shared_ptr<AsuTransport>(std::move(nextTransport));
    return Status::OK();
}

Status AsuClientImpl::BindRegisteredResources(AsuId asuId,
                                              const std::shared_ptr<AsuTransport>& transport)
{
    std::vector<RegisteredResource> resources;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        resources = registeredResources_;
    }
    if (resources.empty()) { return Status::OK(); }

    std::vector<RegisteredMemory> registeredRegions;
    registeredRegions.reserve(resources.size());
    for (const auto& resource : resources) {
        RegisteredMemory registeredRegion;
        registeredRegion.region = resource.region;
        registeredRegion.handle = resource.result.handle;
        registeredRegions.emplace_back(registeredRegion);
    }

    std::vector<RegisterResult> results;
    auto status = transport->BindRegisteredRegions(registeredRegions, results);
    if (!status.ok()) {
        return WithContext(status, "asuId=" + std::to_string(asuId) +
                                       " resource_count=" + std::to_string(resources.size()));
    }
    return Status::OK();
}

Status AsuClientImpl::RefreshView()
{
    AsuClientConfig config;
    std::shared_ptr<ViewSnapshot> oldSnapshot;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!initialized_) { return NotInitialized(); }
        config = config_;
        oldSnapshot = snapshot_;
    }

    GlobalView view;
    auto status = FetchGlobalView(view);
    if (!status.ok()) { return status; }
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!initialized_) { return NotInitialized(); }
        if (IsStaleViewLocked(view)) { return Status::OK(); }
    }

    std::shared_ptr<ViewSnapshot> nextSnapshot;
    status = BuildSnapshot(config, view, oldSnapshot, nextSnapshot);
    if (!status.ok()) { return status; }

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (!initialized_) { return NotInitialized(); }
        if (IsStaleViewLocked(view)) { return Status::OK(); }
        snapshot_ = std::move(nextSnapshot);
    }

    return Status::OK();
}

Status AsuClientImpl::SubmitEntries(const std::vector<KVBuffer>& entries, TaskId& taskId,
                                    TransportOperation operation)
{
    return RunWithRefreshRetry("submit entries", [&](bool& needRefresh) {
        return SubmitEntriesOnce(entries, taskId, operation, needRefresh);
    });
}

Status AsuClientImpl::SubmitEntriesOnce(const std::vector<KVBuffer>& entries, TaskId& taskId,
                                        TransportOperation operation, bool& needRefresh)
{
    taskId = kInvalidTaskId;
    auto snapshot = GetSnapshot();
    if (!snapshot) { return NotInitialized(); }

    AggregateTask task;
    task.entryCount = entries.size();
    task.viewEpoch = snapshot->view.viewEpoch;
    task.viewId = snapshot->view.viewId;
    auto routes = snapshot->router->RouteEntries(entries);
    for (const auto& route : routes) {
        auto transportIter = snapshot->transports.find(route.first);
        if (transportIter == snapshot->transports.end()) {
            auto status = Status::Error(StatusCode::NOT_FOUND, "routed asu transport not found");
            MarkRefreshIfNeeded(status, needRefresh);
            return WithContext(status, "asuId=" + std::to_string(route.first));
        }

        std::vector<KVBuffer> childEntries;
        childEntries.reserve(route.second.size());
        for (auto index : route.second) { childEntries.emplace_back(entries[index]); }

        TaskId childTaskId = kInvalidTaskId;
        auto status = (transportIter->second.get()->*operation)(childEntries, childTaskId);
        if (!status.ok()) {
            MarkRefreshIfNeeded(status, needRefresh);
            return WithContext(status, "asuId=" + std::to_string(route.first) +
                                           " entryCount=" + std::to_string(childEntries.size()));
        }
        task.childTasks.emplace_back(
            ChildTask{route.first, childTaskId, transportIter->second, route.second});
    }

    taskId = SaveAggregateTask(std::move(task));
    return Status::OK();
}

Status AsuClientImpl::SubmitDelete(const std::vector<CacheKey>& keys, TaskId& taskId)
{
    return RunWithRefreshRetry("submit delete", [&](bool& needRefresh) {
        return SubmitDeleteOnce(keys, taskId, needRefresh);
    });
}

Status AsuClientImpl::SubmitDeleteOnce(const std::vector<CacheKey>& keys, TaskId& taskId,
                                       bool& needRefresh)
{
    taskId = kInvalidTaskId;
    auto snapshot = GetSnapshot();
    if (!snapshot) { return NotInitialized(); }

    AggregateTask task;
    task.entryCount = keys.size();
    task.viewEpoch = snapshot->view.viewEpoch;
    task.viewId = snapshot->view.viewId;
    auto routes = snapshot->router->RouteKeys(keys);
    for (const auto& route : routes) {
        auto transportIter = snapshot->transports.find(route.first);
        if (transportIter == snapshot->transports.end()) {
            auto status = Status::Error(StatusCode::NOT_FOUND, "routed asu transport not found");
            MarkRefreshIfNeeded(status, needRefresh);
            return WithContext(status, "asuId=" + std::to_string(route.first));
        }

        std::vector<CacheKey> childKeys;
        childKeys.reserve(route.second.size());
        for (auto index : route.second) { childKeys.emplace_back(keys[index]); }

        TaskId childTaskId = kInvalidTaskId;
        auto status = transportIter->second->DeleteAsync(childKeys, childTaskId);
        if (!status.ok()) {
            MarkRefreshIfNeeded(status, needRefresh);
            return WithContext(status, "asuId=" + std::to_string(route.first) +
                                           " key_count=" + std::to_string(childKeys.size()));
        }
        task.childTasks.emplace_back(
            ChildTask{route.first, childTaskId, transportIter->second, route.second});
    }

    taskId = SaveAggregateTask(std::move(task));
    return Status::OK();
}

std::shared_ptr<AsuClientImpl::ViewSnapshot> AsuClientImpl::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (!initialized_) { return nullptr; }
    return snapshot_;
}

TaskId AsuClientImpl::SaveAggregateTask(AggregateTask task)
{
    auto taskId = nextTaskId_.fetch_add(1);
    if (taskId == kInvalidTaskId) { taskId = nextTaskId_.fetch_add(1); }

    std::lock_guard<std::mutex> lock{mutex_};
    tasks_.emplace(taskId, std::move(task));
    return taskId;
}

void AsuClientImpl::RemoveAggregateTask(TaskId taskId)
{
    std::lock_guard<std::mutex> lock{mutex_};
    tasks_.erase(taskId);
}

Status AsuClientImpl::CollectTaskResult(const AggregateTask& task, bool wait,
                                        std::uint64_t timeoutMs, TaskResult& result,
                                        bool& needRefresh)
{
    result.status = Status::OK();
    result.entry_status.assign(task.entryCount, Status::OK());
    result.query_result.reset();

    Status finalStatus = Status::OK();
    for (const auto& childTask : task.childTasks) {
        if (childTask.transport == nullptr) {
            auto status = Status::Error(StatusCode::NOT_FOUND, "task transport not found");
            MarkRefreshIfNeeded(status, needRefresh);
            finalStatus = WithContext(PartialFailed("task transport not found"),
                                      "asuId=" + std::to_string(childTask.asuId) +
                                          " childTaskId=" + std::to_string(childTask.taskId) +
                                          " viewEpoch=" + std::to_string(task.viewEpoch) +
                                          " viewId=" + std::to_string(task.viewId));
            continue;
        }

        TaskResult childResult;
        auto status = wait ? childTask.transport->Wait(childTask.taskId, timeoutMs, childResult)
                           : childTask.transport->Check(childTask.taskId, childResult);
        if (!status.ok() || !childResult.status.ok()) {
            if (!status.ok()) { MarkRefreshIfNeeded(status, needRefresh); }
            if (!childResult.status.ok()) { MarkRefreshIfNeeded(childResult.status, needRefresh); }
            if (status.code == StatusCode::IN_PROGRESS ||
                childResult.status.code == StatusCode::IN_PROGRESS) {
                finalStatus =
                    WithContext(Status::Error(StatusCode::IN_PROGRESS, "child task is in progress"),
                                "asuId=" + std::to_string(childTask.asuId) +
                                    " childTaskId=" + std::to_string(childTask.taskId) +
                                    " viewEpoch=" + std::to_string(task.viewEpoch) +
                                    " viewId=" + std::to_string(task.viewId));
            } else if (finalStatus.ok() || finalStatus.code == StatusCode::IN_PROGRESS) {
                finalStatus = WithContext(PartialFailed("one or more child tasks failed"),
                                          "asuId=" + std::to_string(childTask.asuId) +
                                              " childTaskId=" + std::to_string(childTask.taskId) +
                                              " viewEpoch=" + std::to_string(task.viewEpoch) +
                                              " viewId=" + std::to_string(task.viewId));
            }
        }

        for (std::size_t index = 0; index < childTask.entryIndices.size(); ++index) {
            auto entryIndex = childTask.entryIndices[index];
            if (entryIndex >= result.entry_status.size()) { continue; }
            if (index < childResult.entry_status.size()) {
                result.entry_status[entryIndex] = childResult.entry_status[index];
            } else if (!childResult.status.ok()) {
                result.entry_status[entryIndex] = childResult.status;
            }
        }
    }

    result.status = finalStatus;
    return finalStatus;
}

void AsuClientImpl::MarkRefreshIfNeeded(const Status& status, bool& needRefresh)
{
    if (ShouldRefreshView(status)) { needRefresh = true; }
}

std::vector<AsuId> AsuClientImpl::GetSortedAsuIds(const GlobalView& view)
{
    std::vector<AsuId> asuIds;
    asuIds.reserve(view.asuMap.size());
    for (const auto& item : view.asuMap) {
        if (item.first != kInvalidAsuId) { asuIds.emplace_back(item.first); }
    }
    std::sort(asuIds.begin(), asuIds.end());
    return asuIds;
}

GlobalView AsuClientImpl::MakeConfigGlobalView(const AsuClientConfig& config)
{
    GlobalView view;
    for (const auto& transportConfig : config.transportConfigs) {
        AsuInfo asuInfo;
        asuInfo.asuId = transportConfig.asu_id;
        view.asuMap.emplace(transportConfig.asu_id, asuInfo);
    }
    return view;
}

bool AsuClientImpl::HasKnownViewEpoch(const GlobalView& view) { return view.viewEpoch != 0; }

bool AsuClientImpl::ShouldRefreshView(const Status& status)
{
    switch (status.code) {
        case StatusCode::CONNECTION_ERROR:
        case StatusCode::IO_ERROR:
        case StatusCode::TIMEOUT:
        case StatusCode::NOT_FOUND:
        case StatusCode::BUFFER_NOT_REGISTERED: return true;
        default: return false;
    }
}

bool AsuClientImpl::IsTaskComplete(const TaskResult& result)
{
    if (!IsTaskStatusComplete(result.status)) { return false; }
    return std::all_of(result.entry_status.begin(), result.entry_status.end(),
                       [](const Status& status) { return IsTaskStatusComplete(status); });
}

bool AsuClientImpl::IsTaskStatusComplete(const Status& status)
{
    return status.code != StatusCode::IN_PROGRESS && status.code != StatusCode::TIMEOUT;
}

Status AsuClientImpl::WithContext(Status status, const std::string& context)
{
    if (context.empty()) { return status; }
    if (status.message.empty()) {
        status.message = context;
    } else {
        status.message += ", " + context;
    }
    return status;
}

Status AsuClientImpl::NotInitialized()
{
    return Status::Error(StatusCode::NOT_INITIALIZED, "asu client is not initialized");
}

std::unique_ptr<AsuClient> CreateAsuClient(TransportFactory factory)
{
    return std::make_unique<AsuClientImpl>(std::move(factory));
}

}  // namespace UC::ASU
