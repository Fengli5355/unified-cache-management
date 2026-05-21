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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include "asu_client/asu_client.h"
#include "client_task_manager.h"
#include "asu_transport/asu_transport.h"
#include "kv_common/router.h"

namespace UC::ASU {

using TransportFactory = std::function<std::unique_ptr<AsuTransport>()>;
using HashFunction = UC::KV::HashFunction;
using HashTableConfig = UC::KV::HashTableConfig;
using HashTableType = UC::KV::HashTableType;

constexpr std::uint64_t kDefaultVirtualNodeCount = UC::KV::kDefaultVirtualNodeCount;
constexpr std::uint64_t kDefaultMaglevTableSize = UC::KV::kDefaultMaglevTableSize;

// AsuIps lists the IP addresses available on one ASU disk.
using AsuIps = std::vector<std::string>;

// GlobalView carries the routing membership and view metadata.
struct GlobalView {
    std::uint64_t viewEpoch{0};
    std::uint64_t viewId{0};
    std::unordered_map<AsuId, AsuIps> asuMap;
    std::uint64_t createTimeMs{0};
    std::uint64_t expireTimeMs{0};
};

// ViewServer fetches the newest global view for the client.
class ViewServer {
public:
    // Destroys the view server interface.
    virtual ~ViewServer() = default;
    // Fetches the current global view.
    virtual Status GetGlobalView(GlobalView& view) = 0;
};

// AsuClientConfig contains all client initialization dependencies.
struct AsuClientConfig {
    std::string clientId;
    std::vector<std::string> viewServiceAddrs;
    std::shared_ptr<ViewServer> viewServer;

    std::vector<TransportConfig> transportConfigs;

    HashFunction hash;
    HashTableConfig hashTable;
    std::uint64_t defaultWaitTimeoutMs{100};
    std::unordered_map<std::string, std::string> attrs;
};

// Creates the default ASU client implementation.
std::unique_ptr<AsuClient> CreateAsuClient(TransportFactory factory = CreateAsuTransport);

// AsuClientImpl coordinates routing, transports, and aggregate task tracking.
class AsuClientImpl final : public AsuClient {
public:
    // Builds a client with the provided transport factory.
    explicit AsuClientImpl(TransportFactory factory);
    // Shuts down the client during destruction.
    ~AsuClientImpl() override;

    // Initializes routing and transport resources.
    Status Init(const AsuClientConfig& config) override;
    // Gracefully drains tracked transport tasks and releases resources.
    Status Shutdown() override;

    // Queries key existence without retrying on current-view failures.
    Status Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                 QueryResult& result) override;

    // Submits load operations to routed transports.
    Status LoadAsync(const std::vector<KVBuffer>& entries, TaskId& taskId) override;
    // Submits store operations to routed transports.
    Status StoreAsync(const std::vector<KVBuffer>& entries, TaskId& taskId) override;
    // Submits delete operations to routed transports.
    Status DeleteAsync(const std::vector<CacheKey>& keys, TaskId& taskId) override;

    // Checks an aggregate task.
    Status Check(TaskId taskId, TaskResult& result) override;
    // Waits for an aggregate task.
    Status Wait(TaskId taskId, std::uint64_t timeoutMs, TaskResult& result) override;

    // Registers regions and remembers successful resources for future views.
    Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                           std::vector<RegisterResult>& results) override;
    // Unregisters regions and forgets successful resources.
    Status UnregisterRegions(const std::vector<MRHandle>& handles) override;

private:
    using EntryIndex = std::size_t;

    // ViewSnapshot is the immutable routing state used by foreground IO.
    struct ViewSnapshot {
        std::shared_ptr<UC::KV::Router> router;
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
    // ChildTask records one transport-level task inside an aggregate task.
    struct ChildTask {
        AsuId asuId{0};
        TaskId taskId{kInvalidTaskId};
        std::shared_ptr<AsuTransport> transport;
        std::vector<EntryIndex> entryIndices;
    };

    // AggregateTask tracks all transport tasks created by one client operation.
    struct AggregateTask {
        std::vector<ChildTask> childTasks;
        std::size_t entryCount{0};
        std::uint64_t viewEpoch{0};
        std::uint64_t viewId{0};
    };

    // RegisteredResource keeps memory metadata that must be rebound after refresh.
    struct RegisteredResource {
        MemoryRegion region;
        RegisterResult result;
    };

    using TransportOperation = Status (AsuTransport::*)(const std::vector<KVBuffer>&, TaskId&);

    // Fetches the next global view from the configured source.
    Status FetchGlobalView(GlobalView& view);
    // Checks whether a view is stale while mutex_ is held.
    bool IsStaleViewLocked(const GlobalView& view) const;
    // Builds a complete immutable snapshot for a view.
    Status BuildSnapshot(const AsuClientConfig& config, const GlobalView& view,
                         const std::shared_ptr<ViewSnapshot>& oldSnapshot,
                         std::shared_ptr<ViewSnapshot>& snapshot);
    // Creates and initializes a transport for one ASU.
    Status BuildTransport(AsuId asuId, std::shared_ptr<AsuTransport>& transport);
    // Binds remembered registered resources to a transport.
    Status BindRegisteredResources(AsuId asuId, const std::shared_ptr<AsuTransport>& transport);
    // Refreshes the view and publishes it if it is newer.
    Status RefreshView();
    // Starts a non-blocking background refresh if one is not already running.
    void RequestBackgroundRefresh();
    // Waits for the background refresh worker to finish.
    void JoinBackgroundRefresh();
    // Shuts down transports owned by a snapshot.
    Status ShutdownSnapshotTransports(const std::shared_ptr<ViewSnapshot>& snapshot);
    // Waits for tracked transport tasks before transport shutdown.
    Status DrainTasksBeforeShutdown(const std::vector<AggregateTask>& tasks,
                                    std::uint64_t waitTimeoutMs);
    // Performs one query attempt on the current snapshot.
    Status QueryOnce(const std::vector<CacheKey>& keys, const QueryOptions& options,
                     QueryResult& result, bool& needRefresh);
    // Performs one register operation on the current snapshot.
    Status RegisterRegionsOnce(const std::vector<MemoryRegion>& regions,
                               std::vector<RegisterResult>& results, bool& needRefresh);
    // Performs one unregister operation on the current snapshot.
    Status UnregisterRegionsOnce(const std::vector<MRHandle>& handles, bool& needRefresh);
    // Submits entries through the selected transport operation.
    Status SubmitEntries(const std::vector<KVBuffer>& entries, TaskId& taskId,
                         TransportOperation operation);
    // Performs one routed entry submission attempt.
    Status SubmitEntriesOnce(const std::vector<KVBuffer>& entries, TaskId& taskId,
                             TransportOperation operation, bool& needRefresh);
    // Submits delete operations through routed transports.
    Status SubmitDelete(const std::vector<CacheKey>& keys, TaskId& taskId);
    // Performs one routed delete submission attempt.
    Status SubmitDeleteOnce(const std::vector<CacheKey>& keys, TaskId& taskId, bool& needRefresh);
    // Returns the current immutable snapshot if initialized.
    std::shared_ptr<ViewSnapshot> GetSnapshot() const;
    // Stores an aggregate task and returns its client task id.
    TaskId SaveAggregateTask(AggregateTask task);
    // Removes a completed aggregate task.
    void RemoveAggregateTask(TaskId taskId);
    // Collects child task results into a client-level result.
    Status CollectTaskResult(const AggregateTask& task, bool wait, std::uint64_t timeoutMs,
                             TaskResult& result, bool& needRefresh);
    // Marks whether a status should trigger background refresh.
    static void MarkRefreshIfNeeded(const Status& status, bool& needRefresh);
    // Extracts sorted ASU ids from a view.
    static std::vector<AsuId> GetSortedAsuIds(const GlobalView& view);
    // Builds a static view from transport configs.
    static GlobalView MakeConfigGlobalView(const AsuClientConfig& config);
    // Returns whether a view carries a comparable epoch.
    static bool HasKnownViewEpoch(const GlobalView& view);
    // Returns whether an operation status implies stale routing.
    static bool ShouldRefreshView(const Status& status);
    // Returns whether all child statuses are terminal.
    static bool IsTaskComplete(const TaskResult& result);
    // Returns whether one task status is terminal.
    static bool IsTaskStatusComplete(const Status& status);
    // Adds context to a status message.
    static Status WithContext(Status status, const std::string& context);
    // Builds the standard not-initialized status.
    static Status NotInitialized();
    TransportFactory transportFactory_;
    // mutex_ protects initialized_, config_, viewServer_, transportConfigs_,
    // registeredResources_, snapshot_, retiredTransports_, tasks_, and refreshInProgress_.
    mutable std::mutex mutex_;
    bool initialized_{false};
    bool refreshInProgress_{false};
    AsuClientConfig config_;
    std::shared_ptr<ViewServer> viewServer_;
    std::unordered_map<AsuId, TransportConfig> transportConfigs_;
    std::vector<RegisteredResource> registeredResources_;
    std::shared_ptr<ViewSnapshot> snapshot_;
    std::vector<std::shared_ptr<AsuTransport>> retiredTransports_;
    std::atomic<TaskId> nextTaskId_{1};
    std::unordered_map<TaskId, AggregateTask> tasks_;
    std::thread refreshThread_;
};

}  // namespace UC::ASU
