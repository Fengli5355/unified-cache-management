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
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include "asu_client/asu_client.h"
#include "client_task_manager.h"

namespace UC::ASU {

class AsuClientImpl final : public AsuClient {
public:
    explicit AsuClientImpl(TransportFactory factory);
    ~AsuClientImpl() override;

    Status Init(const AsuClientConfig& config) override;
    Status Shutdown() override;

    Status Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                 QueryResult& result) override;

    Status LoadAsync(const std::vector<KVBuffer>& entries, TaskId& taskId) override;
    Status StoreAsync(const std::vector<KVBuffer>& entries, TaskId& taskId) override;
    Status DeleteAsync(const std::vector<CacheKey>& keys, TaskId& taskId) override;

    Status Check(TaskId taskId, TaskResult& result) override;
    Status Wait(TaskId taskId, std::uint64_t timeoutMs, TaskResult& result) override;

    Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                           std::vector<RegisterResult>& results) override;
    Status UnregisterRegions(const std::vector<MRHandle>& handles) override;

private:
    using EntryIndex = std::size_t;

    struct Router {
        std::vector<AsuId> asu_ids;

        AsuId Pick(const CacheKey& key) const;
        using RingNode = std::pair<std::uint64_t, AsuId>;

        Router(const std::vector<AsuId>& asuIds, HashFunction hash, HashTableConfig config);

        std::unordered_map<AsuId, std::vector<EntryIndex>> RouteKeys(
            const std::vector<CacheKey>& keys) const;
        std::unordered_map<AsuId, std::vector<EntryIndex>> RouteEntries(
            const std::vector<KVBuffer>& entries) const;

    private:
        AsuId RouteKey(const CacheKey& key) const;
        void BuildRing(const std::vector<AsuId>& asuIds);
        void BuildMaglev(const std::vector<AsuId>& asuIds);

        HashFunction hash_;
        HashTableConfig hashTable_;
        std::vector<RingNode> ring_;
        std::vector<AsuId> lookupTable_;
    };

    struct ViewSnapshot {
        std::shared_ptr<Router> router;
        std::vector<AsuId> asuIds;
        GlobalView view;
        std::unordered_map<AsuId, std::shared_ptr<AsuTransport>> transports;
    };

    Status SubmitAsync(ClientOpType op_type, const std::vector<KVBuffer>& entries, TaskId& task_id);
    using ClientTaskContextPtr = std::shared_ptr<ClientTaskContext>;

    Status DispatchTask(const ClientTaskContextPtr& ctx);
    bool PollTask(const ClientTaskContextPtr& ctx);
    Status BuildResult(const ClientTaskContextPtr& ctx, TaskResult& result);

    TransportFactory transport_factory_;
    AsuClientConfig config_;
    std::shared_ptr<ViewSnapshot> view_;
    ClientTaskManager task_manager_;
    // The following impls are mine.
    struct ChildTask {
        AsuId asuId{0};
        TaskId taskId{kInvalidTaskId};
        std::shared_ptr<AsuTransport> transport;
        std::vector<EntryIndex> entryIndices;
    };

    struct AggregateTask {
        std::vector<ChildTask> childTasks;
        std::size_t entryCount{0};
        std::uint64_t viewEpoch{0};
        std::uint64_t viewId{0};
    };

    struct RegisteredResource {
        MemoryRegion region;
        RegisterResult result;
    };

    using TransportOperation = Status (AsuTransport::*)(const std::vector<KVBuffer>&, TaskId&);

    template <typename Operation>
    Status RunWithRefreshRetry(const std::string& operationName, Operation operation);

    Status FetchGlobalView(GlobalView& view);
    bool IsStaleViewLocked(const GlobalView& view) const;
    Status BuildSnapshot(const AsuClientConfig& config, const GlobalView& view,
                         const std::shared_ptr<ViewSnapshot>& oldSnapshot,
                         std::shared_ptr<ViewSnapshot>& snapshot);
    Status BuildTransport(AsuId asuId, std::shared_ptr<AsuTransport>& transport);
    Status BindRegisteredResources(AsuId asuId, const std::shared_ptr<AsuTransport>& transport);
    Status RefreshView();
    Status QueryOnce(const std::vector<CacheKey>& keys, const QueryOptions& options,
                     QueryResult& result, bool& needRefresh);
    Status RegisterRegionsOnce(const std::vector<MemoryRegion>& regions,
                               std::vector<RegisterResult>& results, bool& needRefresh);
    Status UnregisterRegionsOnce(const std::vector<MRHandle>& handles, bool& needRefresh);
    Status SubmitEntries(const std::vector<KVBuffer>& entries, TaskId& taskId,
                         TransportOperation operation);
    Status SubmitEntriesOnce(const std::vector<KVBuffer>& entries, TaskId& taskId,
                             TransportOperation operation, bool& needRefresh);
    Status SubmitDelete(const std::vector<CacheKey>& keys, TaskId& taskId);
    Status SubmitDeleteOnce(const std::vector<CacheKey>& keys, TaskId& taskId, bool& needRefresh);
    std::shared_ptr<ViewSnapshot> GetSnapshot() const;
    TaskId SaveAggregateTask(AggregateTask task);
    void RemoveAggregateTask(TaskId taskId);
    Status CollectTaskResult(const AggregateTask& task, bool wait, std::uint64_t timeoutMs,
                             TaskResult& result, bool& needRefresh);
    static void MarkRefreshIfNeeded(const Status& status, bool& needRefresh);
    static std::vector<AsuId> GetSortedAsuIds(const GlobalView& view);
    static GlobalView MakeConfigGlobalView(const AsuClientConfig& config);
    static bool HasKnownViewEpoch(const GlobalView& view);
    static bool ShouldRefreshView(const Status& status);
    static bool IsTaskComplete(const TaskResult& result);
    static bool IsTaskStatusComplete(const Status& status);
    static Status WithContext(Status status, const std::string& context);
    static Status NotInitialized();
    TransportFactory transportFactory_;
    mutable std::mutex mutex_;
    bool initialized_{false};
    AsuClientConfig config_;
    std::shared_ptr<ViewServer> viewServer_;
    std::unordered_map<AsuId, TransportConfig> transportConfigs_;
    std::vector<RegisteredResource> registeredResources_;
    std::shared_ptr<ViewSnapshot> snapshot_;
    std::atomic<TaskId> nextTaskId_{1};
    std::unordered_map<TaskId, AggregateTask> tasks_;
};

}  // namespace UC::ASU
