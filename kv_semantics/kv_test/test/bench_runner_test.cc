// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#include "bench_runner.h"
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include "kv_client_runner.h"

namespace kv::bench {
namespace {
struct State {
    int submits{0};
    int waits{0};
    int checks{0};
    int unregisters{0};
    int shutdowns{0};
    bool failSubmit{false};
    bool failWait{true};
    StatusCode waitError{StatusCode::TIMEOUT};
};
class Client final : public KvClient {
public:
    explicit Client(std::shared_ptr<State> state) : state_(std::move(state)) {}
    kv::Status Init(const KvClientConfig&) override { return kv::Status::OK(); }
    kv::Status Shutdown() override
    {
        ++state_->shutdowns;
        return kv::Status::OK();
    }
    kv::Status StoreAsync(const std::vector<KVBuffer>&, TaskId& id,
                          std::function<kv::Status()>) override
    {
        id = ++state_->submits;
        return state_->failSubmit && id == 2
                   ? kv::Status::Error(StatusCode::RESOURCE_BUSY, "submit failed")
                   : kv::Status::OK();
    }
    kv::Status LoadAsync(const std::vector<KVBuffer>& entries, TaskId& id) override
    {
        return StoreAsync(entries, id, {});
    }
    kv::Status QueryAsync(const std::vector<CacheKey>&, TaskId&) override
    {
        return kv::Status::OK();
    }
    kv::Status DeleteAsync(const std::vector<CacheKey>&, TaskId&) override
    {
        return kv::Status::OK();
    }
    bool Check(TaskId) override
    {
        ++state_->checks;
        return false;
    }
    kv::Status Wait(TaskId, std::uint64_t, TaskResult& result) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        result.status = ++state_->waits == 2 && state_->failWait
                            ? kv::Status::Error(state_->waitError, "wait failed")
                            : kv::Status::OK();
        result.entryStatus = {result.status};
        return result.status;
    }
    kv::Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                               std::vector<RegisteredMemory>& registered) override
    {
        for (const auto& region : regions) {
            RegisteredMemory memory;
            memory.region = region;
            memory.handle = registered.size() + 1;
            registered.push_back(memory);
        }
        return kv::Status::OK();
    }
    kv::Status UnregisterRegions(const std::vector<MRHandle>&) override
    {
        ++state_->unregisters;
        return kv::Status::OK();
    }

private:
    std::shared_ptr<State> state_;
};
KvTestConfig Config()
{
    KvTestConfig config;
    TransportConfig transport;
    // No device provider: exercise host payloads without NPU dependencies.
    transport.providerType = TransProviderType::UNSUPPORTED;
    config.asuClientConfig.transportConfigs.push_back(transport);
    config.bench.op = BenchOpType::STORE;
    config.bench.ioSize = 512;
    config.bench.ioIntervalUs = 1;
    config.bench.concurrency = 1;
    config.bench.ioCount = 3;
    config.memoryMaxBytes = 4096;
    return config;
}
void CheckPartialMetrics(const CommandResult& result)
{
    EXPECT_FALSE(result.status.Ok());
    EXPECT_EQ(result.benchMetrics.completedOperations, 2U);
    EXPECT_EQ(result.benchMetrics.completedEntries, 2U);
    EXPECT_EQ(result.benchMetrics.completedBytes, 1024U);
    EXPECT_EQ(result.benchMetrics.errorCount, 1U);
    EXPECT_GT(result.benchMetrics.elapsedSec, 0.0);
    EXPECT_GT(result.benchMetrics.avgBandwidthBytesPerSec, 0.0);
    EXPECT_GT(result.benchMetrics.latency.avgUs, 0.0);
    ASSERT_FALSE(result.benchMetrics.realtimeSamples.empty());
    EXPECT_EQ(result.benchMetrics.realtimeSamples.back().errorCount, 1U);
}
TEST(BenchRunnerTest, TimeoutContinuesAndFinalizesMetrics)
{
    auto state = std::make_shared<State>();
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_FALSE(BenchRunner().Run({}, Config(), runner, result).Ok());
    CheckPartialMetrics(result);
    EXPECT_EQ(result.taskResult.status.code, StatusCode::TIMEOUT);
    EXPECT_EQ(state->submits, 3);
    EXPECT_EQ(state->waits, 3);
    EXPECT_EQ(state->checks, 0);
    EXPECT_EQ(state->unregisters, 1);
    EXPECT_EQ(state->shutdowns, 0);
    EXPECT_TRUE(runner.Shutdown().Ok());
}
TEST(BenchRunnerTest, CompletionErrorContinuesWithoutPolling)
{
    auto state = std::make_shared<State>();
    state->waitError = StatusCode::IO_ERROR;
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_FALSE(BenchRunner().Run({}, Config(), runner, result).Ok());
    CheckPartialMetrics(result);
    EXPECT_EQ(result.taskResult.status.code, StatusCode::IO_ERROR);
    EXPECT_EQ(state->submits, 3);
    EXPECT_EQ(state->waits, 3);
    EXPECT_EQ(state->checks, 0);
    EXPECT_EQ(state->unregisters, 1);
}
TEST(BenchRunnerTest, WarmupFailureContinuesIntoMeasuredPhase)
{
    auto state = std::make_shared<State>();
    KvClientRunner runner(std::make_unique<Client>(state));
    auto config = Config();
    config.bench.warmupSec = 1;
    CommandResult result;
    EXPECT_TRUE(BenchRunner().Run({}, config, runner, result).Ok());
    EXPECT_TRUE(result.status.Ok());
    EXPECT_EQ(state->unregisters, 1);
    EXPECT_EQ(result.benchMetrics.completedOperations, 3U);
    EXPECT_EQ(result.benchMetrics.errorCount, 0U);
    EXPECT_FALSE(result.benchMetrics.realtimeSamples.empty());
}
TEST(BenchRunnerTest, SuccessfulRunUnregistersBuffersAndFinalizesStatistics)
{
    auto state = std::make_shared<State>();
    state->failWait = false;
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_TRUE(BenchRunner().Run({}, Config(), runner, result).Ok());
    EXPECT_EQ(state->unregisters, 1);
    EXPECT_EQ(result.benchMetrics.completedOperations, 3U);
    EXPECT_EQ(result.benchMetrics.completedBytes, 1536U);
    EXPECT_GT(result.benchMetrics.avgBandwidthBytesPerSec, 0.0);
}

TEST(BenchRunnerTest, SubmissionFailureFinalizesPartialMetricsAndUnregisters)
{
    auto state = std::make_shared<State>();
    state->failSubmit = true;
    state->failWait = false;
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_FALSE(BenchRunner().Run({}, Config(), runner, result).Ok());
    CheckPartialMetrics(result);
    EXPECT_EQ(state->waits, 2);
    EXPECT_EQ(state->unregisters, 1);
}
}  // namespace
}  // namespace kv::bench
