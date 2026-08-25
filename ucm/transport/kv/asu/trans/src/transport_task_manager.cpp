#include "transport_task_manager.h"
#include <chrono>
#include <utility>
#include "asu_response_status.h"
#include "logger.h"

namespace UC::ASU {

namespace {

std::int64_t DurationMicroseconds(std::chrono::steady_clock::time_point start,
                                  std::chrono::steady_clock::time_point end)
{
    if (start == std::chrono::steady_clock::time_point{} ||
        end == std::chrono::steady_clock::time_point{}) {
        return -1;
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

}  // namespace

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
    completedAt = std::chrono::steady_clock::now();
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
    TaskResult result;
    BuildResult(*task, result);
    const auto transportStatusCode = task->finalStatus.code;
    if (task->NotifyCompletion(std::move(result))) {
        UC_INFO_UNLIMITED(
            "[ASU_PERF] event=asu_transport_complete client_task_id={} transport_task_id={} "
            "asu_id={} op={} client_age_at_complete_us={} transport_total_us={} "
            "transport_queue_us={} prepare_us={} assign_us={} build_send_us={} "
            "send_setup_us={} send_us={} completion_wait_us={} status={}",
            task->clientTaskId, task->taskId, task->asuId, static_cast<int>(task->opType),
            DurationMicroseconds(task->clientSubmittedAt, task->completedAt),
            DurationMicroseconds(task->submittedAt, task->completedAt),
            DurationMicroseconds(task->submittedAt, task->executeStartedAt), task->prepareUs,
            task->assignUs, task->buildSendUs, task->sendSetupUs, task->sendUs,
            DurationMicroseconds(task->sendEndedAt, task->completedAt),
            static_cast<int>(transportStatusCode));
    }
    (void)Remove(task->taskId);
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

}  // namespace UC::ASU
