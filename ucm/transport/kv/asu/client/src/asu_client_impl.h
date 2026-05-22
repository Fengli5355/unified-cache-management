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
#include "asu_transport/asu_transport.h"
#include "client_task_manager.h"
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

// ViewSnapshot is the immutable routing state used by foreground IO and submitted tasks.
struct ViewSnapshot {
    std::shared_ptr<UC::KV::Router> router;
    std::vector<AsuId> asuIds;
    GlobalView view;
    std::unordered_map<AsuId, std::shared_ptr<AsuTransport>> transports;
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
    // Gracefully drains tracked client tasks and releases resources.
    Status Shutdown() override;

    // Queries key existence and schedules background refresh on refreshable failures.
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
    using ClientTaskContextPtr = std::shared_ptr<ClientTaskContext>;

    // RegisteredResource keeps memory metadata that must be rebound after refresh.
    struct RegisteredResource {
        MemoryRegion region;
        RegisterResult result;
    };

    // Submits one entry-based client task through the refresh-retry wrapper.
    Status SubmitAsync(ClientOpType opType, const std::vector<KVBuffer>& entries, TaskId& taskId);
    // Builds and dispatches one entry-based task attempt on the current snapshot.
    Status SubmitAsyncOnce(ClientOpType opType, const std::vector<KVBuffer>& entries,
                           TaskId& taskId, bool& needRefresh);
    // Submits one key-based client task through the refresh-retry wrapper.
    Status SubmitAsync(ClientOpType opType, const std::vector<CacheKey>& keys, TaskId& taskId);
    // Builds and dispatches one key-based task attempt on the current snapshot.
    Status SubmitAsyncOnce(ClientOpType opType, const std::vector<CacheKey>& keys, TaskId& taskId,
                           bool& needRefresh);

    // Sends each subtask to its routed transport and records transport task ids.
    Status DispatchTask(const ClientTaskContextPtr& ctx);
    // Polls transport subtasks and copies completed entry statuses back by original index.
    bool PollTask(const ClientTaskContextPtr& ctx);
    // Converts a client task context into the public task result shape.
    Status BuildResult(const ClientTaskContextPtr& ctx, TaskResult& result);
    // Waits for one client task context until completion or timeout.
    Status WaitTaskContext(const ClientTaskContextPtr& ctx, std::uint64_t timeoutMs,
                           TaskResult& result);

    // Performs one query attempt on the current snapshot.
    Status QueryOnce(const std::vector<CacheKey>& keys, const QueryOptions& options,
                     QueryResult& result, bool& needRefresh);
    // Performs one register operation on the current snapshot.
    Status RegisterRegionsOnce(const std::vector<MemoryRegion>& regions,
                               std::vector<RegisterResult>& results, bool& needRefresh);
    // Performs one unregister operation on the current snapshot.
    Status UnregisterRegionsOnce(const std::vector<MRHandle>& handles, bool& needRefresh);

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
    // Returns the current immutable snapshot if initialized.
    std::shared_ptr<ViewSnapshot> GetSnapshot() const;

    // Refreshes the view and publishes it if it is newer.
    Status RefreshView();
    // Starts a non-blocking refresh after a status indicates stale routing or transport state.
    void RequestBackgroundRefresh();
    // Waits for the background refresh worker to finish.
    void JoinBackgroundRefresh();

    // Shuts down transports owned by a snapshot.
    Status ShutdownSnapshotTransports(const std::shared_ptr<ViewSnapshot>& snapshot);
    // Waits for tracked client tasks before transport shutdown.
    Status DrainTasksBeforeShutdown(std::uint64_t waitTimeoutMs);

    // Marks whether a status suggests the published snapshot should be refreshed.
    static void MarkRefreshIfNeeded(const Status& status, bool& needRefresh);
    // Extracts sorted ASU ids from a view.
    static std::vector<AsuId> GetSortedAsuIds(const GlobalView& view);
    // Builds a static view from transport configs.
    static GlobalView MakeConfigGlobalView(const AsuClientConfig& config);
    // Returns whether a view carries a comparable epoch.
    static bool HasKnownViewEpoch(const GlobalView& view);
    // Returns whether an operation status should schedule view refresh.
    static bool ShouldRefreshView(const Status& status);
    // Returns whether any task status should schedule view refresh.
    static bool ShouldRefreshView(const TaskResult& result);
    // Returns whether all child statuses are terminal.
    static bool IsTaskComplete(const TaskResult& result);
    // Returns whether one task status is terminal.
    static bool IsTaskStatusComplete(const Status& status);
    // Adds context to a status message.
    static Status WithContext(Status status, const std::string& context);
    // Builds the standard not-initialized status.
    static Status NotInitialized();

    // Tracks aggregate client tasks returned through public TaskId values.
    ClientTaskManager taskManager_;
    // Creates ASU transports; tests inject fake transports through this hook.
    TransportFactory transportFactory_;
    // mutex_ protects background refresh state and resource/view caches.
    mutable std::mutex mutex_;
    // Tracks whether Init has published a usable snapshot.
    bool initialized_{false};
    // Prevents duplicate background refresh workers.
    bool refreshInProgress_{false};
    // Last accepted initialization config.
    AsuClientConfig config_;
    // Source for dynamic global views; may be backed by viewServiceAddrs.
    std::shared_ptr<ViewServer> viewServer_;
    // Transport configs indexed by ASU id for snapshot construction.
    std::unordered_map<AsuId, TransportConfig> transportConfigs_;
    // Resources registered on the current view and rebound to newly added transports.
    std::vector<RegisteredResource> registeredResources_;
    // Current immutable routing and transport snapshot.
    std::shared_ptr<ViewSnapshot> snapshot_;
    // Transports removed from the active snapshot but still needed by old tasks.
    std::vector<std::shared_ptr<AsuTransport>> retiredTransports_;
    // Worker used for non-blocking refresh requests.
    std::thread refreshThread_;
};

}  // namespace UC::ASU
