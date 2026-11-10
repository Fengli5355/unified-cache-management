// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#include "bench_runner.h"
#include "arg_parser.h"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <map>
#include <set>
#include <thread>
#include "kv_client_runner.h"

namespace kv::bench {
namespace {
struct State {
    std::map<TaskId, std::chrono::steady_clock::time_point> started;
    std::map<TaskId, std::uint64_t> addresses;
    std::set<CacheKey> keys;
    std::size_t peakInflight{0};
    std::uint64_t completionDelayUs{0};
    bool neverCompletes{false};
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
    kv::Status StoreAsync(const std::vector<KVBuffer>& entries, TaskId& id,
                          std::function<kv::Status()>) override
    {
        id = ++state_->submits;
        if (state_->failSubmit && id == 2) {
            return kv::Status::Error(StatusCode::RESOURCE_BUSY, "submit failed");
        }
        const auto now = std::chrono::steady_clock::now();
        state_->started[id] = now;
        for (const auto& entry : entries) { state_->keys.insert(entry.key); }
        if (!entries.empty()) {
            for (const auto& active : state_->addresses) {
                EXPECT_NE(active.second, entries.front().buffer.region.addr);
            }
            state_->addresses[id] = entries.front().buffer.region.addr;
        }
        state_->peakInflight = std::max(state_->peakInflight, state_->started.size());
        return kv::Status::OK();
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
    bool Check(TaskId id) override
    {
        ++state_->checks;
        return !state_->neverCompletes &&
            std::chrono::steady_clock::now() - state_->started.at(id) >=
                std::chrono::microseconds(state_->completionDelayUs);
    }
    kv::Status Wait(TaskId id, std::uint64_t, TaskResult& result) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        state_->started.erase(id);
        state_->addresses.erase(id);
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
TEST(BenchRunnerTest, AsyncCompletesOverlappingRequestsWithIndependentBuffers)
{
    auto state = std::make_shared<State>();
    state->failWait = false;
    state->completionDelayUs = 6000;
    auto config = Config();
    config.bench.executionMode = "async";
    config.bench.ioIntervalUs = 2000;
    config.asuClientConfig.defaultWaitTimeoutMs = 10;
    config.bench.ioCount = 12;
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_TRUE(BenchRunner().Run({}, config, runner, result).Ok());
    EXPECT_EQ(result.benchMetrics.concurrency, 6U);
    EXPECT_GT(state->peakInflight, 1U);
    EXPECT_LE(state->peakInflight, 6U);
    EXPECT_EQ(state->waits, 12);
    EXPECT_TRUE(state->started.empty());
    EXPECT_EQ(state->unregisters, 6);

}
TEST(BenchRunnerTest, AsyncExpiryReusesSlotsAndContinuesSubmitting)
{
    auto state = std::make_shared<State>();
    state->neverCompletes = true;
    auto config = Config();
    config.bench.executionMode = "async";
    config.bench.ioIntervalUs = 1000;
    config.asuClientConfig.defaultWaitTimeoutMs = 4;
    config.bench.ioCount = 15;
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_FALSE(BenchRunner().Run({}, config, runner, result).Ok());
    EXPECT_EQ(result.benchMetrics.concurrency, 5U);
    EXPECT_EQ(result.benchMetrics.errorCount, 15U);
    EXPECT_EQ(result.benchMetrics.completedBytes, 0U);
    EXPECT_EQ(state->submits, 15);
    EXPECT_EQ(state->waits, 15);
    EXPECT_LE(state->peakInflight, 5U);
    EXPECT_TRUE(state->started.empty());
    EXPECT_EQ(state->unregisters, 5);
}
TEST(BenchRunnerTest, UniqueKeysDoNotWrapWhileSyncReusesOneBuffer)
{
    auto state = std::make_shared<State>();
    state->failWait = false;
    auto config = Config();
    config.bench.keyMode = "unique";
    config.bench.ioCount = 40;
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_TRUE(BenchRunner().Run({}, config, runner, result).Ok());
    EXPECT_EQ(state->keys.size(), 40U);
    EXPECT_EQ(state->peakInflight, 1U);
    EXPECT_EQ(state->checks, 0);
    EXPECT_EQ(state->unregisters, 1);
}
TEST(BenchRunnerTest, AsyncRejectsPoolAboveMemoryLimitBeforeSubmitting)
{
    auto state = std::make_shared<State>();
    auto config = Config();
    config.bench.executionMode = "async";
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_FALSE(BenchRunner().Run({}, config, runner, result).Ok());
    EXPECT_EQ(state->submits, 0);
}
TEST(BenchRunnerTest, AsyncPartialBatchUsesRegisteredSlotPrefix)
{
    auto state = std::make_shared<State>();
    state->failWait = false;
    auto config = Config();
    config.bench.executionMode = "async";
    config.bench.op = BenchOpType::BATCH_STORE;
    config.bench.batchSize = 2;
    config.bench.ioIntervalUs = 2000;
    config.asuClientConfig.defaultWaitTimeoutMs = 2;
    config.bench.ioCount = 3;
    KvClientRunner runner(std::make_unique<Client>(state));
    CommandResult result;
    EXPECT_TRUE(BenchRunner().Run({}, config, runner, result).Ok());
    EXPECT_EQ(result.benchMetrics.completedEntries, 3U);
    EXPECT_EQ(state->submits, 2);
}
TEST(BenchRunnerTest, ParsesExecutionKeyModeAndIntervalWithBenchPrefix)
{
    std::vector<std::string> args = {"kv-test", "bench", "store", "--execution-mode",
        "async", "--key-mode", "unique", "--io-interval-us", "2000", "--prefix", "run1", "--count", "20"};
    std::vector<char*> argv;
    for (auto& arg : args) { argv.push_back(arg.data()); }
    CommandOptions options;
    ASSERT_TRUE(ArgParser().Parse(static_cast<int>(argv.size()), argv.data(), options).Ok());
    EXPECT_EQ(options.executionMode, "async");
    EXPECT_EQ(options.keyMode, "unique");
    EXPECT_EQ(options.ioIntervalUs, 2000U);
    EXPECT_EQ(options.ioCount, 20U);
    EXPECT_EQ(options.keyPrefix, "run1");
    args[4] = "invalid";
    argv.clear();
    for (auto& arg : args) { argv.push_back(arg.data()); }
    EXPECT_FALSE(ArgParser().Parse(static_cast<int>(argv.size()), argv.data(), options).Ok());
}
}  // namespace
}  // namespace kv::bench
