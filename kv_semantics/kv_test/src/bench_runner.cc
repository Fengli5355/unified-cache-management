// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#include "bench_runner.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <thread>
#include "buffer_allocator.h"
#include "key_value_generator.h"
#include "kv_metrics/metrics.h"
#include "kv_test_config_helpers.h"
#include "payload_buffer_runtime.h"

namespace kv::bench {

std::string FormatMiBPerSec(double bytesPerSec)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << (bytesPerSec / (1024.0 * 1024.0));
    return stream.str();
}

namespace {

constexpr int kExitInvalidArgument = 1;
constexpr int kExitAsuStatusBase = 100;

struct BenchBufferSlot {
    BufferSet buffers;
};

struct OperationOutcome {
    Status status;
    double latencyUs{0.0};
    std::size_t entryCount{0};
    std::uint64_t bytes{0};
    bool isRead{false};
    kv::TaskResult taskResult;
};

using BenchBufferPool = std::vector<BenchBufferSlot>;

std::uint64_t BenchEntryCount(const BenchConfig& bench, BenchOpType op)
{
    if (op == BenchOpType::BATCH_STORE || op == BenchOpType::BATCH_RETRIEVE ||
        op == BenchOpType::MIX) {
        return std::max<std::uint32_t>(bench.batchSize, 1);
    }
    return 1;
}

double PercentileUs(const std::vector<double>& sortedLatenciesUs, double percentile)
{
    if (sortedLatenciesUs.empty()) { return 0.0; }
    const double rank =
        std::ceil((percentile / 100.0) * static_cast<double>(sortedLatenciesUs.size()));
    const auto index = static_cast<std::size_t>(
        std::min<double>(std::max<double>(rank, 1.0), sortedLatenciesUs.size()) - 1.0);
    return sortedLatenciesUs[index];
}

BenchLatencyStats BuildLatencyStats(const std::vector<double>& latenciesUs)
{
    BenchLatencyStats stats;
    if (latenciesUs.empty()) { return stats; }

    auto sortedLatenciesUs = latenciesUs;
    std::sort(sortedLatenciesUs.begin(), sortedLatenciesUs.end());
    stats.minUs = sortedLatenciesUs.front();
    stats.maxUs = sortedLatenciesUs.back();
    stats.avgUs = std::accumulate(sortedLatenciesUs.begin(), sortedLatenciesUs.end(), 0.0) /
                  static_cast<double>(sortedLatenciesUs.size());
    stats.p99_9Us = PercentileUs(sortedLatenciesUs, 99.9);
    stats.p99_99Us = PercentileUs(sortedLatenciesUs, 99.99);
    stats.p99_999Us = PercentileUs(sortedLatenciesUs, 99.999);
    return stats;
}

std::string FormatUs(double latencyUs)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << latencyUs;
    return stream.str();
}

void PrintProgressSample(const BenchRealtimeSample& sample, std::uint64_t operationsPerSec)
{
    std::cout << '[' << sample.timestampSec << "s] ops=" << operationsPerSec
              << " entries/s=" << static_cast<std::uint64_t>(sample.iops)
              << " bw=" << FormatMiBPerSec(sample.bandwidthBytesPerSec)
              << "MiB/s avg=" << FormatUs(sample.avgLatencyUs) << "us"
              << " err=" << sample.errorCount << '\n';
}

Status CheckBenchMemoryLimit(std::uint64_t entryCount, std::uint64_t ioSize,
                             std::uint64_t memoryMaxBytes)
{
    if (memoryMaxBytes == 0) {
        return Status::Error(kExitInvalidArgument,
                             "limits.memory_max_bytes must be greater than zero");
    }
    if (entryCount == 0 || ioSize == 0) { return Status::Success(); }
    if (entryCount > std::numeric_limits<std::uint64_t>::max() / ioSize) {
        return Status::Error(kExitInvalidArgument, "bench buffer pool bytes overflow uint64");
    }

    const auto requiredBytes = entryCount * ioSize;
    if (requiredBytes > memoryMaxBytes) {
        return Status::Error(kExitInvalidArgument,
                             "bench buffer pool bytes exceed limits.memory_max_bytes: required=" +
                                 std::to_string(requiredBytes) +
                                 ", limit=" + std::to_string(memoryMaxBytes));
    }
    return Status::Success();
}

Status ValidateBenchBufferConfig(const BenchConfig& bench, std::uint64_t poolEntryCount,
                                 std::uint64_t memoryMaxBytes)
{
    if (bench.ioSize == 0) {
        return Status::Error(kExitInvalidArgument, "bench io_size must be greater than zero");
    }
    if (poolEntryCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Status::Error(kExitInvalidArgument,
                             "bench buffer pool entry count exceeds addressable memory");
    }
    if (bench.ioSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Status::Error(kExitInvalidArgument, "bench io_size exceeds addressable memory");
    }

    return CheckBenchMemoryLimit(poolEntryCount, bench.ioSize, memoryMaxBytes);
}

void FillStoreValue(std::vector<std::uint8_t>& value, std::uint64_t valueIndex, std::uint64_t seed)
{
    for (std::size_t byteIndex = 0; byteIndex < value.size(); ++byteIndex) {
        value[byteIndex] = static_cast<std::uint8_t>((valueIndex + byteIndex + seed) & 0xFF);
    }
}

Status SyncBenchDeviceBuffers(const KvTestConfig& config, BenchBufferSlot& slot,
                              std::size_t entryCount)
{
    auto status = MaybeSetUpPayloadThread(config);
    if (!status.Ok()) { return status; }

    auto& buffers = slot.buffers;
    if (buffers.ownedBuffers.size() < entryCount || buffers.deviceBuffers.empty() ||
        buffers.deviceBufferOffsets.size() < entryCount) {
        return Status::Error(kExitInvalidArgument, "device payload bench buffer count mismatch");
    }
    const auto baseAddr = reinterpret_cast<std::uintptr_t>(buffers.deviceBuffers.front().get());
    for (std::size_t index = 0; index < entryCount; ++index) {
        status = CopyHostToDevice(buffers.ownedBuffers[index],
                                  baseAddr + buffers.deviceBufferOffsets[index],
                                  "bench index=" + std::to_string(index));
        if (!status.Ok()) { return status; }
    }
    return Status::Success();
}

Status BuildBenchBufferPool(const KvTestConfig& config, bool useDeviceBuffers,
                            std::uint64_t entryCountPerOperation, std::size_t slotCount,
                            BenchBufferPool& pool)
{
    const auto& bench = config.bench;
    const auto allocationPolicy = AllocationPolicyForConfig(config);
    if (useDeviceBuffers) {
        auto status = MaybeSetUpPayloadThread(config);
        if (!status.Ok()) { return status; }
    }

    pool.resize(slotCount);
    for (std::size_t slotIndex = 0; slotIndex < pool.size(); ++slotIndex) {
        auto& buffers = pool[slotIndex].buffers;
        buffers.ownedBuffers.reserve(static_cast<std::size_t>(entryCountPerOperation));
        buffers.deviceBufferOffsets.reserve(static_cast<std::size_t>(entryCountPerOperation));
        std::size_t deviceBufferSize = 0;
        for (std::uint64_t index = 0; index < entryCountPerOperation; ++index) {
            auto& value = buffers.ownedBuffers.emplace_back(static_cast<std::size_t>(bench.ioSize));
            FillStoreValue(value, slotIndex * entryCountPerOperation + index, config.bench.seed);
            if (useDeviceBuffers) {
                const auto offset = AlignUp(deviceBufferSize, DeviceBufferAlignment());
                if (offset < deviceBufferSize ||
                    value.size() > std::numeric_limits<std::size_t>::max() - offset) {
                    return Status::Error(kExitInvalidArgument,
                                         "device payload bench buffer size overflow");
                }
                buffers.deviceBufferOffsets.emplace_back(offset);
                deviceBufferSize = offset + value.size();
            }
        }
        if (useDeviceBuffers) {
            const auto registerAlignment = DeviceAllocationAlignment(allocationPolicy);
            const auto registerSize = AlignUp(deviceBufferSize, registerAlignment);
            if (registerSize < deviceBufferSize) {
                return Status::Error(kExitInvalidArgument,
                                     "device payload bench register size overflow");
            }
            std::shared_ptr<void> deviceBuffer;
            auto status = AllocateDeviceBuffer(registerSize, allocationPolicy, deviceBuffer);
            if (!status.Ok()) { return status; }
            buffers.deviceBuffers.emplace_back(std::move(deviceBuffer));
        }
    }
    return Status::Success();
}

Status SyncStoreBenchDeviceBuffers(const KvTestConfig& config, BenchBufferPool& pool,
                                   std::size_t entryCount)
{
    for (auto& slot : pool) {
        auto status = SyncBenchDeviceBuffers(config, slot, entryCount);
        if (!status.Ok()) { return status; }
    }
    return Status::Success();
}

bool IsBenchReadOperation(BenchOpType op, std::uint64_t operationIndex, const BenchConfig& bench)
{
    if (op == BenchOpType::RETRIEVE || op == BenchOpType::BATCH_RETRIEVE) { return true; }
    if (op == BenchOpType::STORE || op == BenchOpType::BATCH_STORE) { return false; }

    const auto ratioTotal = bench.readRatio + bench.writeRatio;
    if (ratioTotal == 0) { return true; }
    return operationIndex % ratioTotal < bench.readRatio;
}

Status PrepareBenchBuffers(BenchBufferSlot& slot, std::uint64_t begin, std::size_t entryCount,
                           const std::string& keyPrefix, bool useDeviceBuffers,
                           DeviceAllocationPolicy allocationPolicy, std::int32_t logicalDeviceId)
{
    auto& buffers = slot.buffers;
    buffers.regions.clear();
    buffers.entries.clear();
    buffers.entryRegionIndexes.clear();
    buffers.entries.reserve(entryCount);
    if (useDeviceBuffers) {
        const auto baseAddr =
            buffers.deviceBuffers.empty()
                ? 0
                : reinterpret_cast<std::uintptr_t>(buffers.deviceBuffers.front().get());
        const auto dataSize = buffers.ownedBuffers.empty() ? 0
                                                           : buffers.deviceBufferOffsets.back() +
                                                                 buffers.ownedBuffers.back().size();
        const auto regionAlignment = DeviceAllocationAlignment(allocationPolicy);
        const auto regionSize = AlignUp(dataSize, regionAlignment);
        buffers.regions.emplace_back(MakeDeviceRegion(baseAddr, regionSize, logicalDeviceId));
        buffers.entryRegionIndexes.assign(entryCount, 0);
    } else {
        buffers.regions.reserve(entryCount);
    }

    for (std::size_t index = 0; index < entryCount; ++index) {
        const auto keyIndex = begin + index;
        const auto region =
            useDeviceBuffers
                ? MakeDeviceRegion(
                      reinterpret_cast<std::uintptr_t>(buffers.deviceBuffers.front().get()) +
                          buffers.deviceBufferOffsets[index],
                      buffers.ownedBuffers[index].size(), logicalDeviceId)
                : MakeHostRegion(buffers.ownedBuffers[index]);
        if (!useDeviceBuffers) { buffers.regions.emplace_back(region); }
        kv::CacheKey key{};
        auto status =
            StringToCacheKey(keyPrefix + std::to_string(keyIndex), "bench generated", key);
        if (!status.Ok()) { return status; }
        buffers.entries.emplace_back(MakeKvBuffer(key, region));
    }
    return Status::Success();
}

Status BindRegisteredBuffers(BufferSet& buffers)
{
    if (buffers.registeredRegions.size() < buffers.regions.size()) {
        return Status::Error(kExitInvalidArgument,
                             "registered buffer result count does not match region count");
    }
    for (std::size_t entryIndex = 0; entryIndex < buffers.entries.size(); ++entryIndex) {
        const auto regionIndex = buffers.entryRegionIndexes.empty()
                                     ? entryIndex
                                     : buffers.entryRegionIndexes[entryIndex];
        if (regionIndex >= buffers.registeredRegions.size()) {
            return Status::Error(kExitInvalidArgument,
                                 "registered buffer region index out of range");
        }
        buffers.entries[entryIndex].buffer.handle = buffers.registeredRegions[regionIndex].handle;
    }
    return Status::Success();
}

Status RegisterBenchBuffers(KvClientRunner& clientRunner, BenchBufferPool& pool,
                            std::uint64_t entryCountPerOperation, const std::string& keyPrefix,
                            bool useDeviceBuffers, DeviceAllocationPolicy allocationPolicy,
                            std::int32_t logicalDeviceId)
{
    for (std::size_t slotIndex = 0; slotIndex < pool.size(); ++slotIndex) {
        auto& slot = pool[slotIndex];
        auto status =
            PrepareBenchBuffers(slot, slotIndex * entryCountPerOperation,
                                static_cast<std::size_t>(entryCountPerOperation), keyPrefix,
                                useDeviceBuffers, allocationPolicy, logicalDeviceId);
        if (!status.Ok()) { return status; }
        status = clientRunner.RegisterBuffers(slot.buffers);
        if (!status.Ok()) { return status; }
    }
    return Status::Success();
}

Status UnregisterBenchBuffers(KvClientRunner& clientRunner, BenchBufferPool& pool)
{
    Status finalStatus = Status::Success();
    for (auto& slot : pool) {
        auto& buffers = slot.buffers;
        if (buffers.registeredRegions.empty()) { continue; }
        auto status = clientRunner.UnregisterBuffers(buffers);
        if (finalStatus.Ok() && !status.Ok()) { finalStatus = status; }
        buffers.registeredRegions.clear();
    }
    return finalStatus;
}

Status SubmitBenchOperation(BenchOpType requestedOp, const KvTestConfig& config,
                            KvClientRunner& clientRunner, BenchBufferSlot& slot,
                            std::uint64_t begin, std::size_t entryCount,
                            std::uint64_t operationIndex, const std::string& keyPrefix,
                            bool useDeviceBuffers, kv::TaskId& taskId)
{
    const auto& bench = config.bench;
    const bool isRead = IsBenchReadOperation(requestedOp, operationIndex, bench);
    const auto allocationPolicy = AllocationPolicyForConfig(config);
    const auto logicalDeviceId = ResolvePayloadDeviceId(config);
    const auto submitMode =
        entryCount > 1 ? SubmitMode::ALL_ENTRIES_IN_ONE_CALL : SubmitMode::SINGLE_ENTRY_PER_CALL;
    auto status = PrepareBenchBuffers(slot, begin, entryCount, keyPrefix, useDeviceBuffers,
                                      allocationPolicy, logicalDeviceId);
    if (!status.Ok()) { return status; }
    auto& buffers = slot.buffers;

    if (useDeviceBuffers && !isRead && requestedOp == BenchOpType::MIX) {
        auto status = SyncBenchDeviceBuffers(config, slot, entryCount);
        if (!status.Ok()) { return status; }
    }

    status = BindRegisteredBuffers(buffers);
    if (!status.Ok()) { return status; }

    return isRead ? clientRunner.RetrieveAsync(buffers, submitMode, taskId)
                  : clientRunner.StoreAsync(buffers, submitMode, taskId);
}

OperationOutcome WaitBenchOperation(const KvTestConfig& config, KvClientRunner& clientRunner,
                                    kv::TaskId taskId,
                                    std::chrono::steady_clock::time_point operationStart,
                                    std::size_t entryCount, bool isRead,
                                    std::uint64_t waitTimeoutMs = 0)
{
    CommandResult operationResult;
    auto opStatus =
        clientRunner.Wait(taskId, waitTimeoutMs == 0 ? config.asuClientConfig.defaultWaitTimeoutMs
                                                   : waitTimeoutMs, operationResult);
    const auto operationEnd = std::chrono::steady_clock::now();

    OperationOutcome outcome;
    outcome.status = opStatus;
    outcome.taskResult = std::move(operationResult.taskResult);
    outcome.latencyUs =
        std::chrono::duration<double, std::micro>(operationEnd - operationStart).count();
    outcome.entryCount = entryCount;
    outcome.bytes = entryCount * config.bench.ioSize;
    outcome.isRead = isRead;
    return outcome;
}

}  // namespace

Status BenchRunner::Run(const CommandOptions& options, const KvTestConfig& config,
                        KvClientRunner& clientRunner, CommandResult& result) const
{
    const auto& bench = config.bench;
    if (bench.op == BenchOpType::UNKNOWN) {
        return Status::Error(kExitInvalidArgument, "bench op is required");
    }
    if (bench.concurrency == 0) {
        return Status::Error(kExitInvalidArgument, "bench concurrency must be greater than zero");
    }
    if (bench.ioIntervalUs == 0) {
        return Status::Error(kExitInvalidArgument,
                             "bench io_interval_us must be greater than zero");
    }
    if (bench.ioIntervalUs >
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::microseconds::rep>::max())) {
        return Status::Error(kExitInvalidArgument, "bench io_interval_us exceeds supported range");
    }
    if (bench.durationSec == 0 && bench.ioCount == 0) {
        return Status::Error(kExitInvalidArgument,
                             "bench duration or IO count must be greater than zero");
    }
    if (bench.op == BenchOpType::MIX && bench.readRatio + bench.writeRatio == 0) {
        return Status::Error(kExitInvalidArgument,
                             "bench mix requires read_ratio or write_ratio greater than zero");
    }
    if (bench.readRatio > 100 || bench.writeRatio > 100) {
        return Status::Error(kExitInvalidArgument,
                             "bench read_ratio and write_ratio must be in range 0..100");
    }
    if (bench.op == BenchOpType::MIX && bench.readRatio + bench.writeRatio != 100) {
        return Status::Error(kExitInvalidArgument,
                             "bench mix read_ratio and write_ratio must sum to 100");
    }

    const auto entryCountPerOperation = BenchEntryCount(bench, bench.op);
    if (bench.executionMode != "sync" && bench.executionMode != "async") {
        return Status::Error(kExitInvalidArgument, "bench execution_mode must be sync or async");
    }
    if (bench.keyMode != "cycle" && bench.keyMode != "unique") {
        return Status::Error(kExitInvalidArgument, "bench key_mode must be cycle or unique");
    }
    const bool asynchronous = bench.executionMode == "async";
    const auto timeoutMs = config.asuClientConfig.defaultWaitTimeoutMs;
    if (timeoutMs == 0 || timeoutMs > static_cast<std::uint64_t>(
            std::numeric_limits<std::chrono::microseconds::rep>::max()) / 1000) {
        return Status::Error(kExitInvalidArgument, "bench timeout must be positive and representable");
    }
    const auto timeoutUs = timeoutMs * 1000;
    const auto baseSlots = timeoutUs / bench.ioIntervalUs + (timeoutUs % bench.ioIntervalUs != 0);
    const auto slotCount = asynchronous ? baseSlots + baseSlots / 10 + (baseSlots % 10 != 0) : 1;
    if (slotCount > std::numeric_limits<std::uint32_t>::max() ||
        entryCountPerOperation > std::numeric_limits<std::uint64_t>::max() / slotCount) {
        return Status::Error(kExitInvalidArgument, "bench in-flight buffer count exceeds supported range");
    }
    const auto poolEntryCount = entryCountPerOperation * slotCount;
    const auto keyCount = bench.cycleKeyCount == 0 ? entryCountPerOperation * 16
                                                   : bench.cycleKeyCount;

    auto status = ValidateBenchBufferConfig(bench, poolEntryCount, config.memoryMaxBytes);
    if (!status.Ok()) { return status; }
    if (bench.ioCount != 0 &&
        bench.ioCount > std::numeric_limits<std::uint64_t>::max() / bench.ioSize) {
        return Status::Error(kExitInvalidArgument, "bench total IO bytes overflow uint64");
    }

    const std::string keyPrefix = bench.keyPrefix.empty() ? "b" : bench.keyPrefix;
    const bool useDeviceBuffers = UsesDevicePayloadBuffers(config);
    const auto allocationPolicy = AllocationPolicyForConfig(config);
    const auto logicalDeviceId = ResolvePayloadDeviceId(config);
    BenchBufferPool bufferPool;
    status = BuildBenchBufferPool(config, useDeviceBuffers, entryCountPerOperation,
                                  static_cast<std::size_t>(slotCount), bufferPool);
    if (!status.Ok()) { return status; }
    if (useDeviceBuffers &&
        (bench.op == BenchOpType::STORE || bench.op == BenchOpType::BATCH_STORE)) {
        status = SyncStoreBenchDeviceBuffers(config, bufferPool,
                                             static_cast<std::size_t>(entryCountPerOperation));
        if (!status.Ok()) { return status; }
    }
    status = RegisterBenchBuffers(clientRunner, bufferPool, entryCountPerOperation, keyPrefix,
                                  useDeviceBuffers, allocationPolicy, logicalDeviceId);
    if (!status.Ok()) {
        (void)UnregisterBenchBuffers(clientRunner, bufferPool);
        return status;
    }

    using Clock = std::chrono::steady_clock;
    std::vector<double> measuredLatenciesUs;
    std::uint64_t operationIndex = 0;
    std::uint64_t nextUniqueKey = 0;
    std::uint64_t nextCycleKey = 0;
    result = CommandResult{};
    result.benchMetrics.op = bench.op;
    result.benchMetrics.valueSize = bench.ioSize;
    result.benchMetrics.batchSize = static_cast<std::uint32_t>(entryCountPerOperation);
    result.benchMetrics.concurrency = static_cast<std::uint32_t>(slotCount);
    result.benchMetrics.warmupSec = bench.warmupSec;
    result.benchMetrics.durationSec = bench.durationSec;

    auto runPhase = [&](std::uint64_t durationSec, std::uint64_t ioCount,
                        bool collectStats) -> Status {
        Status phaseStatus = Status::Success();
        const auto phaseStart = Clock::now();
        const auto phaseEnd = phaseStart + std::chrono::seconds(durationSec);
        auto nextSubmit = phaseStart;
        const auto ioInterval = std::chrono::microseconds(bench.ioIntervalUs);
        std::uint64_t scheduledEntryCount = 0;
        std::uint64_t windowOperationCount = 0;
        std::uint64_t windowEntryCount = 0;
        std::uint64_t windowBytes = 0;
        std::uint64_t windowErrors = 0;
        double windowLatencyUs = 0.0;
        std::uint64_t currentSecond = 1;
        auto windowStartedAt = phaseStart;

        auto emitProgressSample = [&](std::uint64_t operationsPerSec) {
            BenchRealtimeSample sample;
            sample.timestampSec = currentSecond;
            sample.op = bench.op;
            const auto sampleAt = Clock::now();
            const double windowSeconds =
                std::chrono::duration<double>(sampleAt - windowStartedAt).count();
            sample.bandwidthBytesPerSec = windowSeconds > 0.0 ? windowBytes / windowSeconds : 0.0;
            sample.iops = windowSeconds > 0.0 ? windowEntryCount / windowSeconds : 0.0;
            sample.avgLatencyUs = windowOperationCount == 0
                                      ? 0.0
                                      : windowLatencyUs / static_cast<double>(windowOperationCount);
            sample.errorCount = windowErrors;
            result.benchMetrics.realtimeSamples.emplace_back(sample);
            if (options.progress) {
                PrintProgressSample(
                    sample, windowSeconds > 0.0
                                ? static_cast<std::uint64_t>(operationsPerSec / windowSeconds)
                                : 0);
            }
            windowStartedAt = sampleAt;
        };

        auto advanceProgressWindow = [&]() {
            const auto elapsedSec =
                std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - phaseStart)
                    .count() +
                1;
            if (static_cast<std::uint64_t>(elapsedSec) != currentSecond) {
                emitProgressSample(windowOperationCount);
                currentSecond = static_cast<std::uint64_t>(elapsedSec);
                windowOperationCount = 0;
                windowEntryCount = 0;
                windowBytes = 0;
                windowErrors = 0;
                windowLatencyUs = 0.0;
            }
        };

        auto recordOutcome = [&](const OperationOutcome& outcome) {
            if (!outcome.status.Ok()) {
                if (collectStats) {
                    ++result.benchMetrics.errorCount;
                    ++windowErrors;
                    advanceProgressWindow();
                }
                return;
            }
            if (!collectStats) { return; }

            measuredLatenciesUs.push_back(outcome.latencyUs);
            ++result.benchMetrics.completedOperations;
            result.benchMetrics.completedEntries += outcome.entryCount;
            result.benchMetrics.completedBytes += outcome.bytes;
            metrics::UpdateStats(outcome.isRead ? KV_METRIC("kv_test_bench_retrieve_bytes_total")
                                                : KV_METRIC("kv_test_bench_store_bytes_total"),
                                 static_cast<double>(outcome.bytes));
            ++windowOperationCount;
            windowEntryCount += outcome.entryCount;
            windowBytes += outcome.bytes;
            windowLatencyUs += outcome.latencyUs;
            advanceProgressWindow();
        };

        bool stopSubmitting = false;
        auto shouldContinue = [&]() {
            if (stopSubmitting) { return false; }
            return ioCount == 0 ? Clock::now() < phaseEnd : scheduledEntryCount < ioCount;
        };

        struct PendingOperation {
            kv::TaskId taskId;
            Clock::time_point startedAt;
            std::size_t entryCount;
            std::size_t slot;
            bool isRead;
        };
        std::vector<PendingOperation> pending;
        std::deque<std::size_t> availableSlots;
        for (std::size_t slot = 0; slot < bufferPool.size(); ++slot) {
            availableSlots.push_back(slot);
        }
        const auto timeout = std::chrono::microseconds(timeoutUs);
        const auto finish = [&](const PendingOperation& operation, bool expired) {
            // Expired requests are consumed once. The Client/Server timeout contract
            // permits immediate slot reuse; never poll an expired task again.
            auto outcome = WaitBenchOperation(config, clientRunner, operation.taskId,
                operation.startedAt, operation.entryCount, operation.isRead, expired ? 1 : timeoutMs);
            if (expired) {
                outcome.taskResult.status = kv::Status::Error(kv::StatusCode::TIMEOUT,
                                                              "bench request deadline exceeded");
                outcome.taskResult.entryStatus.assign(operation.entryCount, outcome.taskResult.status);
                outcome.status = Status::Error(kExitAsuStatusBase + static_cast<int>(kv::StatusCode::TIMEOUT),
                                               outcome.taskResult.status.message);
            }
            recordOutcome(outcome);
            if (!outcome.status.Ok()) {
                result.taskResult = outcome.taskResult;
                phaseStatus = outcome.status;
            }
            availableSlots.push_back(operation.slot);
        };

        while (shouldContinue() || !pending.empty()) {
            for (auto it = pending.begin(); it != pending.end();) {
                const bool completed = clientRunner.Check(it->taskId);
                const bool expired = !completed && Clock::now() - it->startedAt >= timeout;
                if (expired || completed) {
                    finish(*it, expired);
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
            if (!shouldContinue()) {
                if (pending.empty()) { break; }
            } else if (!availableSlots.empty() && Clock::now() >= nextSubmit) {
                const auto begin = bench.keyMode == "unique" ? nextUniqueKey
                    : nextCycleKey;
                const auto available = bench.keyMode == "unique"
                    ? std::numeric_limits<std::uint64_t>::max() - begin : keyCount - begin;
                const auto remaining = ioCount == 0 ? entryCountPerOperation : ioCount - scheduledEntryCount;
                const auto currentEntryCount = static_cast<std::size_t>(
                    std::min({entryCountPerOperation, available, remaining}));
                if (currentEntryCount == 0) {
                    phaseStatus = Status::Error(kExitInvalidArgument, "bench unique key range exhausted");
                    // Drain outstanding requests before releasing registered buffers.
                    stopSubmitting = true;
                    continue;
                }
                scheduledEntryCount += currentEntryCount;
                if (bench.keyMode == "unique") {
                    nextUniqueKey += currentEntryCount;
                } else {
                    nextCycleKey = (begin + currentEntryCount) % keyCount;
                }
                const auto currentOperationIndex = operationIndex++;
                const bool isRead = IsBenchReadOperation(bench.op, currentOperationIndex, bench);
                const auto slot = availableSlots.front();
                availableSlots.pop_front();
                kv::TaskId taskId{kv::kInvalidTaskId};
                const auto operationStart = Clock::now();
                const auto submitStatus = SubmitBenchOperation(bench.op, config, clientRunner,
                    bufferPool[slot], begin, currentEntryCount, currentOperationIndex,
                    keyPrefix, useDeviceBuffers, taskId);
                // Preserve a minimum interval; never catch up missed slots in a burst.
                nextSubmit = operationStart + ioInterval;
                if (!submitStatus.Ok()) {
                    phaseStatus = submitStatus;
                    availableSlots.push_back(slot);
                    if (collectStats) {
                        ++result.benchMetrics.errorCount;
                        ++windowErrors;
                        advanceProgressWindow();
                    }
                } else if (asynchronous) {
                    pending.push_back({taskId, operationStart, currentEntryCount, slot, isRead});
                } else {
                    finish({taskId, operationStart, currentEntryCount, slot, isRead}, false);
                }
                continue;
            }
            auto wakeAt = pending.empty() ? nextSubmit
                                          : Clock::now() + std::chrono::microseconds(100);
            if (shouldContinue() && !availableSlots.empty()) { wakeAt = std::min(wakeAt, nextSubmit); }
            for (const auto& operation : pending) {
                wakeAt = std::min(wakeAt, operation.startedAt + timeout);
            }
            std::this_thread::sleep_until(wakeAt);
        }

        if (collectStats && (windowOperationCount != 0 || windowErrors != 0)) {
            emitProgressSample(windowOperationCount);
        }

        return phaseStatus;
    };

    if (bench.warmupSec != 0) {
        (void)runPhase(bench.warmupSec, 0, false);
    }

    const auto measureStart = Clock::now();
    status = runPhase(bench.durationSec, bench.ioCount, true);
    const auto measureEnd = Clock::now();
    auto cleanupStatus = UnregisterBenchBuffers(clientRunner, bufferPool);
    if (status.Ok() && !cleanupStatus.Ok()) { status = cleanupStatus; }

    result.benchMetrics.elapsedSec =
        std::chrono::duration<double>(measureEnd - measureStart).count();
    if (result.benchMetrics.elapsedSec > 0.0) {
        result.benchMetrics.avgBandwidthBytesPerSec =
            static_cast<double>(result.benchMetrics.completedBytes) /
            result.benchMetrics.elapsedSec;
        result.benchMetrics.avgIops = static_cast<double>(result.benchMetrics.completedEntries) /
                                      result.benchMetrics.elapsedSec;
        result.benchMetrics.avgBatchIops =
            static_cast<double>(result.benchMetrics.completedOperations) /
            result.benchMetrics.elapsedSec;
    }
    result.benchMetrics.latency = BuildLatencyStats(measuredLatenciesUs);
    if (status.Ok()) { result.taskResult = BuildEmptyTaskResult(); }
    result.status = status;
    return result.status;
}

}  // namespace kv::bench
