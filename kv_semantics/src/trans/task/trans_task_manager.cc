// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#include "trans_task_manager.h"
#include <chrono>
#include <utility>
#include "kv_metrics/metrics.h"
#include "utils/trans_task_utils.h"

namespace kv {

void FillEntryStatusFromCqeResult(const KvResponse& response,
                                  TransportSubBatchContext& subBatchContext)
{
    FillEntryStatusFromCqeResult(response, subBatchContext.opType, subBatchContext.useSeekControl,
                                 subBatchContext.status, subBatchContext.entryStatus);
}

TransportTask::TransportTask() : subBatchContexts(std::make_shared<TransportSubBatchList>()) {}

bool TransportTask::Done() const
{
    return state.load(std::memory_order_acquire) == TransportTaskState::COMPLETED;
}

bool TransportTask::NotifyCompletion(TaskResult result)
{
    if (!onComplete || completionNotified.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    onComplete(std::move(result));
    return true;
}

Status TransportTask::BuildFinalStatus() const
{
    for (const auto& subBatchContext : *subBatchContexts) {
        if (!subBatchContext.status.ok()) {
            return Status::Error(StatusCode::PARTIAL_FAILED, "transport task partially failed");
        }
    }

    return Status::OK();
}

void TransportTask::InitializeRemainingSubBatchCount()
{
    remainingSubBatchCount = 0;
    for (const auto& subBatchContext : *subBatchContexts) {
        if (subBatchContext.state == TransportSubBatchState::PENDING) { ++remainingSubBatchCount; }
    }
}

void TransportTask::TryFinalizeFromSubBatches()
{
    if (subBatchContexts->empty()) {
        finalStatus = Status::Error(StatusCode::PARTIAL_FAILED, "transport task partially failed");
        state.store(TransportTaskState::COMPLETED, std::memory_order_release);
        return;
    }

    if (remainingSubBatchCount != 0) { return; }

    finalStatus = BuildFinalStatus();
    state.store(TransportTaskState::COMPLETED, std::memory_order_release);
}

void TransportTaskManager::NotifyCompletion(const TransportTaskPtr& task)
{
    const bool sendReturned = task->sendReturned.load(std::memory_order_acquire);
    const auto completedAt = std::chrono::steady_clock::now();
    const metrics::MetricUpdate updates[] = {
        {KV_METRIC("kv_transport_task_e2e_duration_seconds"),
         std::chrono::duration<double>(completedAt - task->submittedAt).count()},
        {KV_METRIC("kv_transport_task_completion_duration_seconds"),
         sendReturned ? std::chrono::duration<double>(completedAt - task->sendCompletedAt).count()
                      : 0.0                                                    },
        {task->asuCompletionMetric,
         sendReturned ? std::chrono::duration<double>(completedAt - task->sendCompletedAt).count()
                      : 0.0                                                    },
    };
    metrics::UpdateStats(updates, sendReturned ? std::size_t{3} : std::size_t{1});
    if (metrics::IsEnabled()) {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (sendReturned && task->firstPollAt != std::chrono::steady_clock::time_point{}) {
            metrics::UpdateStats(
                KV_METRIC("kv_transport_completion_first_poll_duration_seconds"),
                std::chrono::duration<double>(task->firstPollAt - task->sendCompletedAt).count());
            const metrics::MetricUpdate pollUpdates[] = {
                {KV_METRIC("kv_transport_completion_poll_gap_seconds"),       task->maxPollGapSeconds},
                {KV_METRIC("kv_transport_completion_poll_lock_wait_seconds"),
                 task->pollLockWaitSeconds                                                           },
            };
            metrics::UpdateStats(pollUpdates, std::size(pollUpdates));
        }
        // Partial observations on timeout are not a complete response-wait sample.
        if (sendReturned && !task->subBatchContexts->empty() &&
            task->responsesObserved == task->subBatchContexts->size()) {
            const metrics::MetricUpdate responseUpdates[] = {
                {KV_METRIC("kv_transport_completion_response_wait_seconds"),
                 std::chrono::duration<double>(task->lastResponseObservedAt - task->sendCompletedAt)
                     .count()                },
                {KV_METRIC("kv_transport_completion_response_process_seconds"),
                 task->responseProcessSeconds},
            };
            metrics::UpdateStats(responseUpdates, std::size(responseUpdates));
        }
    }
    const auto notifyStartedAt =
        sendReturned ? metrics::StartMetricTimer() : metrics::MetricTimer{};
    TaskResult result;
    BuildResult(*task, result);
    (void)task->NotifyCompletion(std::move(result));
    (void)Remove(task->taskId);
    if (const auto seconds = metrics::ElapsedSeconds(notifyStartedAt)) {
        metrics::UpdateStats(KV_METRIC("kv_transport_completion_notify_duration_seconds"),
                             *seconds);
    }
}

void TransportTaskManager::BuildResult(const TransportTask& task, TaskResult& result)
{
    result.status = task.finalStatus;
    result.entryStatus = task.entryStatus;
    if (!task.subBatchContexts->empty()) {
        std::size_t resultIndex = 0;
        for (const auto& subBatchContext : *task.subBatchContexts) {
            for (const auto& status : subBatchContext.entryStatus) {
                if (resultIndex >= result.entryStatus.size()) { break; }
                result.entryStatus[resultIndex++] = status;
            }
        }
    }

    result.queryResult.reset();
    if (task.opType == AsuOpType::QUERY) {
        result.queryResult = BuildQueryResultFromEntryStatus(result.entryStatus);
    }
}

}  // namespace kv
