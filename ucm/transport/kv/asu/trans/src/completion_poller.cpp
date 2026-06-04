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
#include "completion_poller.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace UC::ASU {

CompletionPoller::CompletionPoller(CompletionPollerConfig config) : config_(config) {}

CompletionPoller::~CompletionPoller() { (void)Stop(); }

Status CompletionPoller::SetIsCompleteFunc(IsCompleteFunc isCompleteFunc)
{
    if (!isCompleteFunc) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "completion check callback is empty");
    }

    std::lock_guard<std::mutex> lock{configMu_};
    auto status = SetCallbackFlag(isCompleteFuncSet_, "completion check callback");
    if (!status.ok()) { return status; }

    isCompleteFunc_ = std::move(isCompleteFunc);
    return Status::OK();
}

Status CompletionPoller::SetCompleteFunc(CompleteFunc completeFunc)
{
    if (!completeFunc) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "completion callback is empty");
    }

    std::lock_guard<std::mutex> lock{configMu_};
    auto status = SetCallbackFlag(completeFuncSet_, "completion callback");
    if (!status.ok()) { return status; }

    completeFunc_ = std::move(completeFunc);
    return Status::OK();
}

Status CompletionPoller::SetCleanupFunc(CleanupFunc cleanupFunc)
{
    if (!cleanupFunc) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "cleanup callback is empty");
    }

    std::lock_guard<std::mutex> lock{configMu_};
    auto status = SetCallbackFlag(cleanupFuncSet_, "cleanup callback");
    if (!status.ok()) { return status; }

    cleanupFunc_ = std::move(cleanupFunc);
    return Status::OK();
}

Status CompletionPoller::Start()
{
    hasStarted_.store(true, std::memory_order_release);

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return Status::OK();
    }

    stop_.store(false, std::memory_order_release);
    pollThread_ = std::thread(&CompletionPoller::PollLoop, this);
    return Status::OK();
}

Status CompletionPoller::Stop()
{
    if (!running_.load(std::memory_order_acquire)) { return Status::OK(); }

    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (pollThread_.joinable()) { pollThread_.join(); }

    std::vector<CompletionWatchEntry> pendingAdds;
    {
        std::lock_guard<std::mutex> lock{pendingMu_};
        pendingAdds.swap(pendingAdds_);
        pendingCount_.store(0, std::memory_order_release);
    }

    CleanupEntries(activeEntries_);
    CleanupEntries(pendingAdds);
    running_.store(false, std::memory_order_release);
    return Status::OK();
}

Status CompletionPoller::Add(CompletionWatchEntry entry)
{
    if (!running_.load(std::memory_order_acquire)) {
        return Status::Error(StatusCode::NOT_INITIALIZED, "completion poller is not running");
    }

    {
        std::lock_guard<std::mutex> lock{pendingMu_};
        if (stop_.load(std::memory_order_acquire)) {
            return Status::Error(StatusCode::NOT_INITIALIZED, "completion poller is stopping");
        }
        pendingAdds_.emplace_back(std::move(entry));
        pendingCount_.fetch_add(1, std::memory_order_release);
    }
    cv_.notify_one();
    return Status::OK();
}

std::size_t CompletionPoller::ActiveCount() const
{
    return activeCount_.load(std::memory_order_acquire);
}

std::size_t CompletionPoller::PendingCount() const
{
    return pendingCount_.load(std::memory_order_acquire);
}

bool CompletionPoller::DefaultIsComplete(const CompletionWatchEntry& entry)
{
    if (entry.completedCid == nullptr) { return false; }
    return *entry.completedCid == entry.expectedCid;
}

Status CompletionPoller::SetCallbackFlag(std::atomic_bool& flag, const std::string& name)
{
    if (hasStarted_.load(std::memory_order_acquire)) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, name + " must be set before start");
    }

    bool expected = false;
    if (!flag.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, name + " is already set");
    }
    return Status::OK();
}

void CompletionPoller::PollLoop()
{
    std::uint32_t noProgressRounds = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        auto hasNewEntries = DrainPendingAdds();
        auto hasCompletedEntries = PollActiveEntries();

        if (activeEntries_.empty()) {
            noProgressRounds = 0;
            std::unique_lock<std::mutex> lock{pendingMu_};
            cv_.wait_for(lock, std::chrono::microseconds(config_.idleSleepUs), [this] {
                return stop_.load(std::memory_order_acquire) ||
                       pendingCount_.load(std::memory_order_acquire) != 0;
            });
        } else if (hasNewEntries || hasCompletedEntries) {
            noProgressRounds = 0;
        } else {
            Backoff(noProgressRounds);
        }
    }
    (void)DrainPendingAdds();
}

bool CompletionPoller::DrainPendingAdds()
{
    if (pendingCount_.load(std::memory_order_acquire) == 0) { return false; }

    std::vector<CompletionWatchEntry> pendingAdds;
    {
        std::lock_guard<std::mutex> lock{pendingMu_};
        pendingAdds.swap(pendingAdds_);
        pendingCount_.fetch_sub(pendingAdds.size(), std::memory_order_acq_rel);
    }
    if (pendingAdds.empty()) { return false; }

    activeEntries_.reserve(activeEntries_.size() + pendingAdds.size());
    for (auto& entry : pendingAdds) { activeEntries_.emplace_back(std::move(entry)); }
    activeCount_.store(activeEntries_.size(), std::memory_order_release);
    return true;
}

bool CompletionPoller::PollActiveEntries()
{
    bool hasCompletedEntries = false;
    const auto maxPollEntries =
        config_.maxPollEntriesPerRound == 0
            ? activeEntries_.size()
            : std::min<std::size_t>(config_.maxPollEntriesPerRound, activeEntries_.size());

    for (std::size_t checked = 0; checked < maxPollEntries && !activeEntries_.empty();) {
        if (pollCursor_ >= activeEntries_.size()) { pollCursor_ = 0; }
        auto index = pollCursor_;
        auto& entry = activeEntries_[index];
        if (!isCompleteFunc_ || !isCompleteFunc_(entry)) {
            ++pollCursor_;
            ++checked;
            continue;
        }

        if (completeFunc_) { (void)completeFunc_(entry); }
        if (cleanupFunc_) { cleanupFunc_(entry); }
        hasCompletedEntries = true;

        if (index + 1 < activeEntries_.size()) {
            activeEntries_[index] = std::move(activeEntries_.back());
        }
        activeEntries_.pop_back();
        if (pollCursor_ >= activeEntries_.size()) { pollCursor_ = 0; }
        activeCount_.store(activeEntries_.size(), std::memory_order_release);
        ++checked;
    }
    return hasCompletedEntries;
}

void CompletionPoller::Backoff(std::uint32_t& noProgressRounds)
{
    ++noProgressRounds;
    if (noProgressRounds <= config_.activeSpinRounds) { return; }

    const auto yieldLimit = config_.activeSpinRounds + config_.activeYieldRounds;
    if (noProgressRounds <= yieldLimit) {
        std::this_thread::yield();
        return;
    }

    if (config_.activeSleepUs > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(config_.activeSleepUs));
    }
}

void CompletionPoller::CleanupEntries(std::vector<CompletionWatchEntry>& entries)
{
    if (cleanupFunc_) {
        for (auto& entry : entries) { cleanupFunc_(entry); }
    }
    entries.clear();
    if (&entries == &activeEntries_) {
        pollCursor_ = 0;
        activeCount_.store(0, std::memory_order_release);
    }
}

}  // namespace UC::ASU
