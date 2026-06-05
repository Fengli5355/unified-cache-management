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
#include "transport_task_completion.h"
#include "buffer_manager.h"
#include "connection_internal.h"
#include "logger.h"

namespace UC::ASU {

namespace {

bool IsSubBatchTerminal(TransportSubBatchState state)
{
    return state == TransportSubBatchState::COMPLETED;
}

Status BuildTaskFinalStatus(const TransportTaskContext& ctx)
{
    for (const auto& subBatchContext : ctx.subBatchContexts) {
        if (!subBatchContext.status.ok()) {
            return Status::Error(StatusCode::PARTIAL_FAILED, "one or more sub-batches failed");
        }
    }

    if (!ctx.finalStatus.ok()) { return ctx.finalStatus; }
    return Status::OK();
}

}  // namespace

void InitializeTerminalSubBatchCount(TransportTaskContext& ctx)
{
    // At submit completion time, terminal sub-batches are usually submit/send failures.
    ctx.completedSubBatchCount = 0;
    for (const auto& subBatchContext : ctx.subBatchContexts) {
        if (!IsSubBatchTerminal(subBatchContext.state)) { continue; }

        ++ctx.completedSubBatchCount;
    }
}

void ReleaseSubBatchResources(TransportSubBatchContext& subBatchContext,
                              BufferManager& sendBufferManager, BufferManager& flagBufferManager)
{
    if (subBatchContext.sendSge.slot_index != UINT32_MAX) {
        const auto slotIndex = subBatchContext.sendSge.slot_index;
        auto status = sendBufferManager.Free(slotIndex);
        if (!status.ok()) {
            UC_ERROR("Failed to release sub-batch send buffer slot({}): {}", slotIndex,
                     status.message);
        }
        subBatchContext.sendSge = {};
    }

    if (subBatchContext.flagBuffer.slot_index != UINT32_MAX) {
        const auto slotIndex = subBatchContext.flagBuffer.slot_index;
        auto status = flagBufferManager.Free(slotIndex);
        if (!status.ok()) {
            UC_ERROR("Failed to release sub-batch flag buffer slot({}): {}", slotIndex,
                     status.message);
        }
        subBatchContext.flagBuffer = {};
    }

    if (subBatchContext.channel != nullptr) {
        subBatchContext.channel->ReleaseInflight();
        subBatchContext.channel = nullptr;
    }
}

void ReleaseAllSubBatchResources(std::vector<TransportSubBatchContext>& subBatchContexts,
                                 BufferManager& sendBufferManager, BufferManager& flagBufferManager)
{
    for (auto& subBatchContext : subBatchContexts) {
        ReleaseSubBatchResources(subBatchContext, sendBufferManager, flagBufferManager);
    }
}

void CompleteSubBatch(TransportTaskContext& ctx, TransportSubBatchContext& subBatchContext,
                      const Status& status, BufferManager& sendBufferManager,
                      BufferManager& flagBufferManager)
{
    if (subBatchContext.state != TransportSubBatchState::PENDING) { return; }

    ReleaseSubBatchResources(subBatchContext, sendBufferManager, flagBufferManager);
    subBatchContext.state = TransportSubBatchState::COMPLETED;
    subBatchContext.status = status;
    ++ctx.completedSubBatchCount;
}

void TryFinalizeTaskFromSubBatches(TransportTaskContext& ctx)
{
    if (ctx.subBatchContexts.empty()) {
        ctx.state.store(TransportTaskState::COMPLETED, std::memory_order_release);
        return;
    }

    if (ctx.completedSubBatchCount != static_cast<std::uint32_t>(ctx.subBatchContexts.size())) {
        return;
    }

    ctx.finalStatus = BuildTaskFinalStatus(ctx);
    ctx.state.store(TransportTaskState::COMPLETED, std::memory_order_release);
}

}  // namespace UC::ASU
