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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "asu_transport/types.h"
#include "transport_task_manager.h"

namespace UC::ASU {

struct CompletionPollerConfig {
    std::uint32_t idleSleepUs{50};
    std::uint32_t activeSpinRounds{64};
    std::uint32_t activeYieldRounds{64};
    std::uint32_t activeSleepUs{1};
    std::uint32_t maxPollEntriesPerRound{256};
};

struct CompletionWatchEntry {
    TaskId taskId{kInvalidTaskId};
    std::uint64_t expectedCid{0};
    const volatile std::uint64_t* completedCid{nullptr};
    std::uint32_t subBatchIndex{0};
    std::shared_ptr<TransportTaskContext> taskContext;
    void* userData{nullptr};
};

class CompletionPoller {
public:
    using IsCompleteFunc = std::function<bool(const CompletionWatchEntry&)>;
    using CompleteFunc = std::function<Status(CompletionWatchEntry&)>;
    using CleanupFunc = std::function<void(CompletionWatchEntry&)>;

    CompletionPoller() = default;
    explicit CompletionPoller(CompletionPollerConfig config);
    ~CompletionPoller();

    CompletionPoller(const CompletionPoller&) = delete;
    CompletionPoller& operator=(const CompletionPoller&) = delete;

    Status SetIsCompleteFunc(IsCompleteFunc isCompleteFunc);
    Status SetCompleteFunc(CompleteFunc completeFunc);
    Status SetCleanupFunc(CleanupFunc cleanupFunc);

    Status Start();
    Status Stop();
    Status Add(CompletionWatchEntry entry);

    std::size_t ActiveCount() const;
    std::size_t PendingCount() const;

private:
    static bool DefaultIsComplete(const CompletionWatchEntry& entry);

    Status SetCallbackFlag(std::atomic_bool& flag, const std::string& name);
    void PollLoop();
    bool DrainPendingAdds();
    bool PollActiveEntries();
    void Backoff(std::uint32_t& noProgressRounds);
    void CleanupEntries(std::vector<CompletionWatchEntry>& entries);

    CompletionPollerConfig config_;

    std::mutex configMu_;
    std::atomic_bool isCompleteFuncSet_{false};
    std::atomic_bool completeFuncSet_{false};
    std::atomic_bool cleanupFuncSet_{false};
    std::atomic_bool hasStarted_{false};

    mutable std::mutex pendingMu_;
    std::condition_variable cv_;
    std::vector<CompletionWatchEntry> pendingAdds_;
    std::atomic_size_t pendingCount_{0};

    std::vector<CompletionWatchEntry> activeEntries_;
    std::size_t pollCursor_{0};
    std::atomic_size_t activeCount_{0};

    std::thread pollThread_;
    std::atomic_bool running_{false};
    std::atomic_bool stop_{false};

    IsCompleteFunc isCompleteFunc_{DefaultIsComplete};
    CompleteFunc completeFunc_;
    CleanupFunc cleanupFunc_;
};

}  // namespace UC::ASU
