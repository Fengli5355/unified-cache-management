#include "asu_client_impl.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UC::ASU {

struct TestState {
    std::uint32_t createdTransports{0};
    bool failFirstQuery{false};
    bool firstQueryFailed{false};
    StatusCode firstQueryFailureCode{StatusCode::CONNECTION_ERROR};
    std::string firstQueryFailureMessage{"fake connection error"};
    bool failFirstLoad{false};
    bool firstLoadFailed{false};
    bool failFirstStore{false};
    bool firstStoreFailed{false};
    bool failStoreAfterFirstDispatch{false};
    std::size_t storeDispatchAttempts{0};
    bool failFirstDelete{false};
    bool firstDeleteFailed{false};
    std::unordered_map<AsuId, Status> queryFailures;
    std::unordered_map<AsuId, Status> loadFailures;
    std::unordered_map<AsuId, Status> storeFailures;
    std::unordered_map<AsuId, Status> deleteFailures;
    std::unordered_map<AsuId, std::vector<Status>> checkEntryStatus;
    std::unordered_map<AsuId, Status> checkResultStatus;
    std::unordered_map<AsuId, QueryResult> prefixQueryResults;
    std::vector<AsuId> registerCalls;
    std::vector<AsuId> bindCalls;
    std::vector<AsuId> unregisterCalls;
    std::vector<AsuId> queryCalls;
    std::unordered_map<AsuId, std::size_t> queryKeyCounts;
    std::unordered_map<AsuId, std::vector<CacheKey>> queryKeys;
    std::vector<AsuId> loadCalls;
    std::vector<AsuId> storeCalls;
    std::vector<AsuId> deleteCalls;
    std::vector<AsuId> checkCalls;
    std::vector<AsuId> waitCalls;
    std::vector<AsuId> cancelCalls;
    std::unordered_map<AsuId, TaskId> childTaskIds;
};

class FakeTransport : public AsuTransport {
public:
    explicit FakeTransport(std::shared_ptr<TestState> state) : state_(std::move(state)) {}

    Status Init(const TransportConfig& config) override
    {
        config_ = config;
        initialized_ = true;
        return Status::OK();
    }

    Status Shutdown() override
    {
        initialized_ = false;
        return Status::OK();
    }

    Status CheckHealth() override { return initialized_ ? Status::OK() : NotInitialized(); }

    Status Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                 QueryResult& result) override
    {
        if (!initialized_) { return NotInitialized(); }
        if (state_->failFirstQuery && !state_->firstQueryFailed) {
            state_->firstQueryFailed = true;
            return Status::Error(state_->firstQueryFailureCode, state_->firstQueryFailureMessage);
        }

        state_->queryCalls.emplace_back(config_.asuId);
        state_->queryKeyCounts[config_.asuId] += keys.size();
        auto& routedKeys = state_->queryKeys[config_.asuId];
        routedKeys.insert(routedKeys.end(), keys.begin(), keys.end());
        auto failureIter = state_->queryFailures.find(config_.asuId);
        if (failureIter != state_->queryFailures.end()) { return failureIter->second; }

        if (options.mode == QueryMode::PREFIX) {
            auto iter = state_->prefixQueryResults.find(config_.asuId);
            if (iter != state_->prefixQueryResults.end()) {
                result = iter->second;
                return Status::OK();
            }
        }

        result.exists.clear();
        result.exists.reserve(keys.size());
        for (const auto& key : keys) { result.exists.emplace_back(key == "k15" || key == "k25"); }
        result.prefixHitKeys = 0;
        return Status::OK();
    }

    Status QueryAsync(const std::vector<CacheKey>&, const QueryOptions&, TaskId& taskId) override
    {
        taskId = 0;
        return Status::OK();
    }

    Status LoadAsync(const std::vector<KVBuffer>&, TaskId& taskId) override
    {
        if (state_->failFirstLoad && !state_->firstLoadFailed) {
            state_->firstLoadFailed = true;
            return Status::Error(StatusCode::CONNECTION_ERROR, "fake load connection error");
        }
        auto failureIter = state_->loadFailures.find(config_.asuId);
        if (failureIter != state_->loadFailures.end()) { return failureIter->second; }

        state_->loadCalls.emplace_back(config_.asuId);
        taskId = 1000 + config_.asuId;
        state_->childTaskIds[config_.asuId] = taskId;
        return Status::OK();
    }

    Status StoreAsync(const std::vector<KVBuffer>&, TaskId& taskId) override
    {
        if (state_->failFirstStore && !state_->firstStoreFailed) {
            state_->firstStoreFailed = true;
            return Status::Error(StatusCode::CONNECTION_ERROR, "fake store connection error");
        }
        auto failureIter = state_->storeFailures.find(config_.asuId);
        if (failureIter != state_->storeFailures.end()) { return failureIter->second; }
        if (state_->failStoreAfterFirstDispatch && ++state_->storeDispatchAttempts > 1) {
            return Status::Error(StatusCode::CONNECTION_ERROR, "fake partial dispatch failure");
        }

        state_->storeCalls.emplace_back(config_.asuId);
        taskId = 2000 + config_.asuId;
        state_->childTaskIds[config_.asuId] = taskId;
        return Status::OK();
    }

    Status DeleteAsync(const std::vector<CacheKey>&, TaskId& taskId) override
    {
        if (state_->failFirstDelete && !state_->firstDeleteFailed) {
            state_->firstDeleteFailed = true;
            return Status::Error(StatusCode::CONNECTION_ERROR, "fake delete connection error");
        }
        auto failureIter = state_->deleteFailures.find(config_.asuId);
        if (failureIter != state_->deleteFailures.end()) { return failureIter->second; }

        state_->deleteCalls.emplace_back(config_.asuId);
        taskId = 3000 + config_.asuId;
        state_->childTaskIds[config_.asuId] = taskId;
        return Status::OK();
    }

    Status Cancel(TaskId) override
    {
        state_->cancelCalls.emplace_back(config_.asuId);
        return Status::OK();
    }

    Status Check(TaskId taskId, TaskResult& result) override
    {
        state_->checkCalls.emplace_back(config_.asuId);
        auto statusIter = state_->checkResultStatus.find(config_.asuId);
        result.status =
            statusIter == state_->checkResultStatus.end() ? Status::OK() : statusIter->second;
        auto entryIter = state_->checkEntryStatus.find(config_.asuId);
        result.entryStatus = entryIter == state_->checkEntryStatus.end()
                                 ? std::vector<Status>{result.status}
                                 : entryIter->second;
        result.queryResult.reset();
        if (taskId == 0) {
            return Status::Error(StatusCode::TASK_NOT_FOUND, "fake task not found");
        }
        return Status::OK();
    }

    Status Wait(TaskId taskId, std::uint64_t, TaskResult& result) override
    {
        state_->waitCalls.emplace_back(config_.asuId);
        return Check(taskId, result);
    }

    Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                           std::vector<RegisterResult>& results) override
    {
        state_->registerCalls.emplace_back(config_.asuId);
        results.clear();
        for (std::size_t index = 0; index < regions.size(); ++index) {
            results.emplace_back(RegisterResult{Status::OK(), 500 + index});
        }
        return Status::OK();
    }

    Status BindRegisteredRegions(const std::vector<RegisteredMemory>& regions,
                                 std::vector<RegisterResult>& results) override
    {
        state_->bindCalls.emplace_back(config_.asuId);
        results.clear();
        for (const auto& region : regions) {
            results.emplace_back(RegisterResult{Status::OK(), region.handle});
        }
        return Status::OK();
    }

    Status UnregisterRegions(const std::vector<MRHandle>&) override
    {
        state_->unregisterCalls.emplace_back(config_.asuId);
        return Status::OK();
    }

private:
    static Status NotInitialized()
    {
        return Status::Error(StatusCode::NOT_INITIALIZED, "fake transport is not initialized");
    }

    std::shared_ptr<TestState> state_;
    TransportConfig config_;
    bool initialized_{false};
};

class FakeViewServer final : public ViewServer {
public:
    explicit FakeViewServer(std::vector<std::vector<AsuId>> views) : views_(std::move(views)) {}

    FakeViewServer(std::vector<std::vector<AsuId>> views, std::vector<std::uint64_t> epochs)
        : views_(std::move(views)), epochs_(std::move(epochs))
    {
    }

    Status GetGlobalView(GlobalView& view) override
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (failFetchAt_ != 0 && fetchCount_ + 1 == failFetchAt_) {
            ++fetchCount_;
            return Status::Error(StatusCode::IO_ERROR, "fake view fetch failed");
        }

        auto index = fetchCount_;
        if (index >= views_.size()) { index = views_.size() - 1; }
        ++fetchCount_;

        view = GlobalView{};
        for (auto asuId : views_[index]) { view.asuMap.emplace(asuId, AsuIps{}); }
        view.viewEpoch = index < epochs_.size() ? epochs_[index] : fetchCount_;
        return Status::OK();
    }

    void FailFetchAt(std::size_t fetchCount)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        failFetchAt_ = fetchCount;
    }
    std::size_t FetchCount() const
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return fetchCount_;
    }

private:
    mutable std::mutex mutex_;
    std::size_t fetchCount_{0};
    std::size_t failFetchAt_{0};
    std::vector<std::vector<AsuId>> views_;
    std::vector<std::uint64_t> epochs_;
};

bool WaitForFetchCount(const std::shared_ptr<FakeViewServer>& viewServer,
                       std::size_t expectedFetchCount)
{
    for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
        if (viewServer->FetchCount() >= expectedFetchCount) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

AsuClientConfig MakeConfig(const std::vector<AsuId>& asuIds)
{
    AsuClientConfig config;
    for (auto asuId : asuIds) {
        TransportConfig transportConfig;
        transportConfig.asuId = asuId;
        config.transportConfigs.emplace_back(std::move(transportConfig));
    }
    config.hashTable.ringHash.virtualNodeCount = 1;
    config.hash = [](const std::string& key) -> std::uint64_t {
        static const std::unordered_map<std::string, std::uint64_t> values{
            {"vn-0#node-10", 10},
            {"vn-0#node-20", 20},
            {"vn-0#node-30", 30},
            {"k05",          5 },
            {"k15",          15},
            {"k25",          25},
            {"k35",          35},
        };
        auto iter = values.find(key);
        if (iter != values.end()) { return iter->second; }
        return 0;
    };
    return config;
}

TransportFactory MakeFactory(const std::shared_ptr<TestState>& state)
{
    return [state] {
        ++state->createdTransports;
        return std::make_unique<FakeTransport>(state);
    };
}

std::uint64_t Crc32IEEE(const std::string& data)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if ((crc & 1U) != 0) {
                    crc = 0xEDB88320U ^ (crc >> 1U);
                } else {
                    crc >>= 1U;
                }
            }
            values[i] = crc;
        }
        return values;
    }();

    std::uint32_t crc = 0xFFFFFFFFU;
    for (unsigned char ch : data) { crc = table[(crc ^ ch) & 0xFFU] ^ (crc >> 8U); }
    return crc ^ 0xFFFFFFFFU;
}

std::uint64_t Fnv1a64(const std::string& value)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t SplitMix64(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t StableHash(const std::string& value) { return SplitMix64(Fnv1a64(value)); }

std::vector<AsuId> MakeAsuIds(std::size_t count)
{
    std::vector<AsuId> asuIds;
    asuIds.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        asuIds.emplace_back(static_cast<AsuId>(index + 1));
    }
    return asuIds;
}

std::vector<CacheKey> MakeKeys(std::size_t count)
{
    std::vector<CacheKey> keys;
    keys.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        keys.emplace_back("distribution-key-" + std::to_string(index));
    }
    return keys;
}

AsuClientConfig MakeDistributionConfig(const std::vector<AsuId>& asuIds, HashTableType type,
                                       HashFunction hashFunc = Crc32IEEE)
{
    auto config = MakeConfig(asuIds);
    config.hash = hashFunc;
    config.hashTable.type = type;
    config.hashTable.ringHash.virtualNodeCount = 256;
    config.hashTable.maglev.tableSize = 65537;
    return config;
}

std::unordered_map<CacheKey, AsuId> CaptureKeyRoutes(const std::vector<AsuId>& asuIds,
                                                     HashTableType type, HashFunction hashFunc,
                                                     const std::vector<CacheKey>& keys)
{
    std::unordered_map<CacheKey, AsuId> routes;
    auto config = MakeDistributionConfig(asuIds, type, std::move(hashFunc));
    config.hashTable.ringHash.virtualNodeCount = 512;
    config.hashTable.maglev.tableSize = 65537;

    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    EXPECT_TRUE(client->Init(config).ok());
    QueryResult result;
    auto status = client->Query(keys, QueryOptions{}, result);
    EXPECT_TRUE(status.ok()) << status.message;

    for (const auto& item : state->queryKeys) {
        for (const auto& key : item.second) { routes.emplace(key, item.first); }
    }
    return routes;
}

double CalculateMigrationRatio(const std::unordered_map<CacheKey, AsuId>& oldRoutes,
                               const std::unordered_map<CacheKey, AsuId>& newRoutes)
{
    std::size_t movedCount = 0;
    std::size_t comparedCount = 0;
    for (const auto& item : oldRoutes) {
        auto iter = newRoutes.find(item.first);
        if (iter == newRoutes.end()) { continue; }
        ++comparedCount;
        if (iter->second != item.second) { ++movedCount; }
    }
    if (comparedCount == 0) { return 0.0; }
    return static_cast<double>(movedCount) / static_cast<double>(comparedCount);
}

void ExpectBalancedDistribution(const std::unordered_map<AsuId, std::size_t>& keyCounts,
                                const std::vector<AsuId>& asuIds, std::size_t totalKeyCount,
                                double maxSkewRatio)
{
    ASSERT_EQ(keyCounts.size(), asuIds.size());

    const auto expectedCount =
        static_cast<double>(totalKeyCount) / static_cast<double>(asuIds.size());
    for (auto asuId : asuIds) {
        auto iter = keyCounts.find(asuId);
        ASSERT_NE(iter, keyCounts.end());
        const auto diff = iter->second > expectedCount ? iter->second - expectedCount
                                                       : expectedCount - iter->second;
        EXPECT_LE(diff / expectedCount, maxSkewRatio)
            << "asuId=" << asuId << " keyCount=" << iter->second << " expected=" << expectedCount;
    }
}

void ExpectSameAsuSet(std::vector<AsuId> actual, std::vector<AsuId> expected)
{
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(actual, expected);
}

TEST(AsuClientImplTest, Lifecycle_OperationsBeforeInitReturnExpectedErrors)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));

    QueryResult queryResult;
    auto status = client->Query({"k05"}, QueryOptions{}, queryResult);
    EXPECT_EQ(status.code, StatusCode::NOT_INITIALIZED);

    TaskId taskId = kInvalidTaskId;
    status = client->StoreAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);
    EXPECT_EQ(status.code, StatusCode::NOT_INITIALIZED);

    TaskResult taskResult;
    status = client->Check(1, taskResult);
    EXPECT_EQ(status.code, StatusCode::TASK_NOT_FOUND);
}

TEST(AsuClientImplTest, Lifecycle_InitTwiceReturnsResourceBusy)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    auto config = MakeConfig({10});
    ASSERT_TRUE(client->Init(config).ok());

    auto status = client->Init(config);

    EXPECT_EQ(status.code, StatusCode::RESOURCE_BUSY);
}

TEST(AsuClientImplTest, Lifecycle_ShutdownClearsTasksAndRejectsFutureOperations)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_TRUE(client->Shutdown().ok());

    QueryResult queryResult;
    status = client->Query({"k05"}, QueryOptions{}, queryResult);
    EXPECT_EQ(status.code, StatusCode::NOT_INITIALIZED);

    TaskResult taskResult;
    status = client->Check(taskId, taskResult);
    EXPECT_EQ(status.code, StatusCode::TASK_NOT_FOUND);
}

TEST(AsuClientImplTest, Input_EmptyQueryReturnsEmptyResult)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    QueryResult result;
    auto status = client->Query({}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_TRUE(result.exists.empty());
    EXPECT_EQ(result.prefixHitKeys, std::uint32_t{0});
    EXPECT_TRUE(state->queryCalls.empty());
}

TEST(AsuClientImplTest, Input_EmptyStoreCreatesCompletableEmptyTask)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->StoreAsync({}, taskId);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_NE(taskId, kInvalidTaskId);
    EXPECT_TRUE(state->storeCalls.empty());

    TaskResult result;
    status = client->Check(taskId, result);
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_TRUE(result.entryStatus.empty());
}

TEST(AsuClientImplTest, Input_EmptyDeleteCreatesCompletableEmptyTask)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->DeleteAsync({}, taskId);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_NE(taskId, kInvalidTaskId);
    EXPECT_TRUE(state->deleteCalls.empty());

    TaskResult result;
    status = client->Check(taskId, result);
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_TRUE(result.entryStatus.empty());
}

TEST(AsuClientImplTest, Input_EmptyRegisterReturnsEmptyResults)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20})).ok());

    std::vector<RegisterResult> results;
    auto status = client->RegisterRegions({}, results);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_TRUE(results.empty());
    EXPECT_EQ(state->registerCalls, std::vector<AsuId>({10}));
    EXPECT_EQ(state->bindCalls, std::vector<AsuId>({20}));
}

TEST(AsuClientImplTest, ViewServer_ConfigFileViewServerLoadsView)
{
    constexpr const char* kConfigPath = "asu_client_impl_view_test.conf";
    {
        std::ofstream configFile{kConfigPath};
        ASSERT_TRUE(configFile.is_open());
        configFile << "viewEpoch=9\n";
        configFile << "viewId=7\n";
        configFile << "asuIds=20\n";
    }

    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20, 30});
    config.viewServiceAddrs = {kConfigPath};
    auto client = CreateAsuClient(MakeFactory(state));
    auto status = client->Init(config);
    std::remove(kConfigPath);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_EQ(state->createdTransports, std::uint32_t{1});

    QueryResult result;
    status = client->Query({"k25"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({1}));
    EXPECT_EQ(state->queryCalls, std::vector<AsuId>({20}));
}

TEST(AsuClientImplTest, ViewServer_InitFailsWhenViewReferencesMissingTransportConfig)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10, 20}
    },
        std::vector<std::uint64_t>{1});
    auto client = CreateAsuClient(MakeFactory(state));

    auto status = client->Init(config);

    EXPECT_EQ(status.code, StatusCode::NOT_FOUND);
    EXPECT_NE(status.message.find("asuId=20"), std::string::npos);
}

TEST(AsuClientImplTest, Query_PerKeyKeepsOriginalOrder)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20})).ok());

    QueryResult result;
    auto status = client->Query({"k05", "k15", "k25"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({0, 1, 1}));
}

TEST(AsuClientImplTest, Query_PerKeyFailureIncludesAsuContext)
{
    auto state = std::make_shared<TestState>();
    state->queryFailures[20] = Status::Error(StatusCode::IO_ERROR, "fake per-key query failure");
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20})).ok());

    QueryResult result;
    auto status = client->Query({"k15"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::IO_ERROR);
    EXPECT_NE(status.message.find("asuId=20"), std::string::npos);
    EXPECT_NE(status.message.find("key_count=1"), std::string::npos);
}

TEST(AsuClientImplTest, Query_PerKeyResultSizeMismatchReturnsInternalError)
{
    class ShortQueryTransport final : public FakeTransport {
    public:
        explicit ShortQueryTransport(std::shared_ptr<TestState> state)
            : FakeTransport(std::move(state))
        {
        }

        Status Query(const std::vector<CacheKey>&, const QueryOptions&,
                     QueryResult& result) override
        {
            result.exists.clear();
            result.prefixHitKeys = 0;
            return Status::OK();
        }
    };

    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient([state] {
        ++state->createdTransports;
        return std::unique_ptr<AsuTransport>(new ShortQueryTransport(state));
    });
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::INTERNAL_ERROR);
    EXPECT_NE(status.message.find("query result size mismatch"), std::string::npos);
    EXPECT_NE(status.message.find("asuId=10"), std::string::npos);
}

TEST(AsuClientImplTest, Query_PrefixBroadcastsAndMergesResults)
{
    auto state = std::make_shared<TestState>();
    state->prefixQueryResults[10] = QueryResult{
        {1, 0, 0},
        2
    };
    state->prefixQueryResults[20] = QueryResult{
        {0, 1, 0},
        3
    };
    state->prefixQueryResults[30] = QueryResult{
        {0, 0, 1},
        5
    };
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20, 30})).ok());

    QueryOptions options;
    options.mode = QueryMode::PREFIX;
    QueryResult result;
    auto status = client->Query({"prefix-a", "prefix-b", "prefix-c"}, options, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({1, 1, 1}));
    EXPECT_EQ(result.prefixHitKeys, std::uint32_t{10});
    ExpectSameAsuSet(state->queryCalls, {10, 20, 30});
    EXPECT_EQ(state->queryKeyCounts[10], std::size_t{3});
    EXPECT_EQ(state->queryKeyCounts[20], std::size_t{3});
    EXPECT_EQ(state->queryKeyCounts[30], std::size_t{3});
}

TEST(AsuClientImplTest, Query_PrefixPartialFailureIncludesAsuContext)
{
    auto state = std::make_shared<TestState>();
    state->prefixQueryResults[10] = QueryResult{
        {1, 0, 0},
        2
    };
    state->queryFailures[20] =
        Status::Error(StatusCode::INVALID_ARGUMENT, "fake prefix query failure");
    state->prefixQueryResults[30] = QueryResult{
        {0, 0, 1},
        5
    };
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20, 30})).ok());

    QueryOptions options;
    options.mode = QueryMode::PREFIX;
    QueryResult result;
    auto status = client->Query({"prefix-a", "prefix-b", "prefix-c"}, options, result);

    EXPECT_EQ(status.code, StatusCode::PARTIAL_FAILED);
    EXPECT_NE(status.message.find("asuId=20"), std::string::npos);
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({1, 0, 1}));
    EXPECT_EQ(result.prefixHitKeys, std::uint32_t{7});
    ExpectSameAsuSet(state->queryCalls, {10, 20, 30});
}

TEST(AsuClientImplTest, Query_PrefixResultSizeMismatchReturnsInternalError)
{
    auto state = std::make_shared<TestState>();
    state->prefixQueryResults[10] = QueryResult{{1}, 1};
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    QueryOptions options;
    options.mode = QueryMode::PREFIX;
    QueryResult result;
    auto status = client->Query({"prefix-a", "prefix-b"}, options, result);

    EXPECT_EQ(status.code, StatusCode::INTERNAL_ERROR);
    EXPECT_NE(status.message.find("prefix query result size mismatch"), std::string::npos);
    EXPECT_NE(status.message.find("asuId=10"), std::string::npos);
}

TEST(AsuClientImplTest, BackgroundRefresh_QueryReturnsErrorWithoutRetry)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20})).ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);

    status = client->Query({"k15"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({1}));
}

TEST(AsuClientImplTest, BackgroundRefresh_QueryDoesNotRefreshNonRefreshableError)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    state->firstQueryFailureCode = StatusCode::INVALID_ARGUMENT;
    state->firstQueryFailureMessage = "fake invalid argument";
    auto viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    auto config = MakeConfig({10, 20});
    config.viewServer = viewServer;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(viewServer->FetchCount(), std::size_t{1});
    EXPECT_EQ(state->createdTransports, std::uint32_t{1});
}

TEST(AsuClientImplTest, BackgroundRefresh_QueryRefreshesOnIoError)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    state->firstQueryFailureCode = StatusCode::IO_ERROR;
    state->firstQueryFailureMessage = "fake io error";
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::IO_ERROR);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
}

TEST(AsuClientImplTest, BackgroundRefresh_QueryRefreshesOnTimeout)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    state->firstQueryFailureCode = StatusCode::TIMEOUT;
    state->firstQueryFailureMessage = "fake timeout";
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::TIMEOUT);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
}

TEST(AsuClientImplTest, BackgroundRefresh_QueryKeepsOriginalErrorWhenViewFetchFails)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    auto viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    viewServer->FailFetchAt(2);
    auto config = MakeConfig({10, 20});
    config.viewServer = viewServer;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{1});
}

TEST(AsuClientImplTest, BackgroundRefresh_LoadReturnsErrorWithoutRetry)
{
    auto state = std::make_shared<TestState>();
    state->failFirstLoad = true;
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->LoadAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    EXPECT_EQ(taskId, kInvalidTaskId);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
    EXPECT_TRUE(state->loadCalls.empty());
}

TEST(AsuClientImplTest, BackgroundRefresh_StoreReturnsErrorWithoutRetry)
{
    auto state = std::make_shared<TestState>();
    state->failFirstStore = true;
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    EXPECT_EQ(taskId, kInvalidTaskId);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
    EXPECT_TRUE(state->storeCalls.empty());
}

TEST(AsuClientImplTest, BackgroundRefresh_DeleteReturnsErrorWithoutRetry)
{
    auto state = std::make_shared<TestState>();
    state->failFirstDelete = true;
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->DeleteAsync({"k05"}, taskId);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    EXPECT_EQ(taskId, kInvalidTaskId);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
    EXPECT_TRUE(state->deleteCalls.empty());
}

TEST(AsuClientImplTest, ViewEpoch_DoesNotPublishSameOrOlderViewEpoch)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    auto viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20},
            {10, 20}
    },
        std::vector<std::uint64_t>{5, 5, 4});
    auto config = MakeConfig({10, 20});
    config.viewServer = viewServer;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);
    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{1});
}

TEST(AsuClientImplTest, SnapshotRefresh_BuildFailureKeepsOldSnapshot)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    auto config = MakeConfig({10});
    auto viewServer = std::make_shared<FakeViewServer>(std::vector<std::vector<AsuId>>{{10}, {20}},
                                                       std::vector<std::uint64_t>{1, 2});
    config.viewServer = viewServer;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);
    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    ASSERT_TRUE(WaitForFetchCount(viewServer, 2));

    status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->createdTransports, std::uint32_t{1});
    EXPECT_EQ(state->queryCalls, std::vector<AsuId>({10}));
}

TEST(AsuClientImplTest, MemoryRegister_RegisterRegionsRegistersFirstTransportAndBindsFollowers)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20, 30})).ok());

    std::vector<RegisterResult> results;
    auto status = client->RegisterRegions({MemoryRegion{}, MemoryRegion{}}, results);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->registerCalls, std::vector<AsuId>({10}));
    EXPECT_EQ(state->bindCalls, std::vector<AsuId>({20, 30}));
    ASSERT_EQ(results.size(), std::size_t{2});
    EXPECT_EQ(results[0].handle, MRHandle{500});
    EXPECT_EQ(results[1].handle, MRHandle{501});
}

TEST(AsuClientImplTest, MemoryRegister_FirstRegisterFailureIncludesAsuContext)
{
    class FailingRegisterTransport final : public FakeTransport {
    public:
        explicit FailingRegisterTransport(std::shared_ptr<TestState> state)
            : FakeTransport(std::move(state))
        {
        }

        Status RegisterRegions(const std::vector<MemoryRegion>&,
                               std::vector<RegisterResult>&) override
        {
            return Status::Error(StatusCode::BUFFER_NOT_REGISTERED, "fake register failure");
        }
    };

    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient([state] {
        ++state->createdTransports;
        return std::unique_ptr<AsuTransport>(new FailingRegisterTransport(state));
    });
    ASSERT_TRUE(client->Init(config).ok());

    std::vector<RegisterResult> results;
    auto status = client->RegisterRegions({MemoryRegion{}}, results);

    EXPECT_EQ(status.code, StatusCode::BUFFER_NOT_REGISTERED);
    EXPECT_NE(status.message.find("asuIndex=0"), std::string::npos);
    EXPECT_NE(status.message.find("asuId=10"), std::string::npos);
    EXPECT_NE(status.message.find("region_count=1"), std::string::npos);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
}

TEST(AsuClientImplTest, MemoryRegister_BindFailureIncludesAsuContext)
{
    class FailingBindTransport final : public FakeTransport {
    public:
        explicit FailingBindTransport(std::shared_ptr<TestState> state)
            : FakeTransport(std::move(state))
        {
        }

        Status BindRegisteredRegions(const std::vector<RegisteredMemory>&,
                                     std::vector<RegisterResult>&) override
        {
            return Status::Error(StatusCode::CONNECTION_ERROR, "fake bind failure");
        }
    };

    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient([state] {
        ++state->createdTransports;
        if (state->createdTransports == 1) {
            return std::unique_ptr<AsuTransport>(new FakeTransport(state));
        }
        return std::unique_ptr<AsuTransport>(new FailingBindTransport(state));
    });
    ASSERT_TRUE(client->Init(MakeConfig({10, 20})).ok());

    std::vector<RegisterResult> results;
    auto status = client->RegisterRegions({MemoryRegion{}}, results);

    EXPECT_EQ(status.code, StatusCode::PARTIAL_FAILED);
    EXPECT_NE(status.message.find("asuIndex=1"), std::string::npos);
    EXPECT_NE(status.message.find("asuId=20"), std::string::npos);
    EXPECT_NE(status.message.find("region_count=1"), std::string::npos);
}

TEST(AsuClientImplTest, MemoryRegister_BindFailureDoesNotCacheResource)
{
    class FailingBindTransport final : public FakeTransport {
    public:
        explicit FailingBindTransport(std::shared_ptr<TestState> state)
            : FakeTransport(state), state_(std::move(state))
        {
        }

        Status BindRegisteredRegions(const std::vector<RegisteredMemory>&,
                                     std::vector<RegisterResult>&) override
        {
            state_->bindCalls.emplace_back(20);
            return Status::Error(StatusCode::CONNECTION_ERROR, "fake bind failure");
        }

    private:
        std::shared_ptr<TestState> state_;
    };

    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20, 30});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10, 20},
            {10, 20, 30}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient([state] {
        ++state->createdTransports;
        if (state->createdTransports == 2) {
            return std::unique_ptr<AsuTransport>(new FailingBindTransport(state));
        }
        return std::unique_ptr<AsuTransport>(new FakeTransport(state));
    });
    ASSERT_TRUE(client->Init(config).ok());

    std::vector<RegisterResult> results;
    auto status = client->RegisterRegions({MemoryRegion{}}, results);
    ASSERT_EQ(status.code, StatusCode::PARTIAL_FAILED);

    state->failFirstQuery = true;
    QueryResult queryResult;
    status = client->Query({"k05"}, QueryOptions{}, queryResult);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{3});
    EXPECT_EQ(state->bindCalls, std::vector<AsuId>({20}));
}

TEST(AsuClientImplTest, MemoryRegister_UnregisterFailureIncludesAsuContext)
{
    class FailingUnregisterTransport final : public FakeTransport {
    public:
        explicit FailingUnregisterTransport(std::shared_ptr<TestState> state)
            : FakeTransport(std::move(state))
        {
        }

        Status UnregisterRegions(const std::vector<MRHandle>&) override
        {
            return Status::Error(StatusCode::IO_ERROR, "fake unregister failure");
        }
    };

    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient([state] {
        ++state->createdTransports;
        return std::unique_ptr<AsuTransport>(new FailingUnregisterTransport(state));
    });
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    auto status = client->UnregisterRegions({7});

    EXPECT_EQ(status.code, StatusCode::IO_ERROR);
    EXPECT_NE(status.message.find("asuId=10"), std::string::npos);
    EXPECT_NE(status.message.find("handle_count=1"), std::string::npos);
}

TEST(AsuClientImplTest, MemoryRegister_UnregisterRemovesCachedResourceBeforeFutureAsuIsAdded)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    std::vector<RegisterResult> results;
    auto status = client->RegisterRegions({MemoryRegion{}}, results);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_EQ(results.size(), std::size_t{1});

    status = client->UnregisterRegions({results[0].handle});
    ASSERT_TRUE(status.ok()) << status.message;

    state->failFirstQuery = true;
    QueryResult queryResult;
    status = client->Query({"k05"}, QueryOptions{}, queryResult);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
    EXPECT_TRUE(state->bindCalls.empty());
}

TEST(AsuClientImplTest, Task_CheckRemovesTaskAfterCompletion)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Check(taskId, result);
    ASSERT_TRUE(status.ok()) << status.message;

    status = client->Check(taskId, result);
    EXPECT_EQ(status.code, StatusCode::TASK_NOT_FOUND);
}

TEST(AsuClientImplTest, Task_CheckKeepsInProgressTaskUntilCompletion)
{
    auto state = std::make_shared<TestState>();
    state->checkResultStatus[10] = Status::Error(StatusCode::IN_PROGRESS, "fake in progress");
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Check(taskId, result);
    EXPECT_EQ(status.code, StatusCode::IN_PROGRESS);

    state->checkResultStatus.erase(10);
    status = client->Check(taskId, result);
    ASSERT_TRUE(status.ok()) << status.message;

    status = client->Check(taskId, result);
    EXPECT_EQ(status.code, StatusCode::TASK_NOT_FOUND);
}

TEST(AsuClientImplTest, Task_CheckRefreshesViewOnRefreshableChildFailure)
{
    auto state = std::make_shared<TestState>();
    state->checkResultStatus[10] = Status::Error(StatusCode::IO_ERROR, "fake child io error");
    auto config = MakeConfig({10, 20});
    auto viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10},
            {10, 20}
    },
        std::vector<std::uint64_t>{1, 2});
    config.viewServer = viewServer;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Check(taskId, result);

    EXPECT_EQ(status.code, StatusCode::PARTIAL_FAILED);
    ASSERT_TRUE(WaitForFetchCount(viewServer, 2));
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
}

TEST(AsuClientImplTest, Task_PartialDispatchFailureCancelsDispatchedSubtasks)
{
    auto state = std::make_shared<TestState>();
    state->failStoreAfterFirstDispatch = true;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20})).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k05", {}},
            KVBuffer{"k15", {}}
    },
        taskId);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    EXPECT_EQ(taskId, kInvalidTaskId);
    ASSERT_EQ(state->storeCalls.size(), std::size_t{1});
    ASSERT_EQ(state->cancelCalls.size(), std::size_t{1});
    EXPECT_EQ(state->cancelCalls[0], state->storeCalls[0]);
}

TEST(AsuClientImplTest, Task_WaitRemovesTaskAfterCompletion)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Wait(taskId, 10, result);
    ASSERT_TRUE(status.ok()) << status.message;

    status = client->Wait(taskId, 10, result);
    EXPECT_EQ(status.code, StatusCode::TASK_NOT_FOUND);
}

TEST(AsuClientImplTest, Task_WaitTimeoutKeepsTaskForLaterCompletion)
{
    class TimeoutOnceTransport final : public FakeTransport {
    public:
        explicit TimeoutOnceTransport(std::shared_ptr<TestState> state)
            : FakeTransport(std::move(state))
        {
        }

        Status Wait(TaskId, std::uint64_t, TaskResult& result) override
        {
            if (!timedOut_) {
                timedOut_ = true;
                result.status = Status::Error(StatusCode::TIMEOUT, "fake wait timeout");
                return result.status;
            }
            return FakeTransport::Check(1000, result);
        }

    private:
        bool timedOut_{false};
    };

    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient([state] {
        ++state->createdTransports;
        return std::unique_ptr<AsuTransport>(new TimeoutOnceTransport(state));
    });
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k05", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Wait(taskId, 10, result);
    EXPECT_EQ(status.code, StatusCode::TIMEOUT);

    status = client->Wait(taskId, 10, result);
    EXPECT_TRUE(status.ok()) << status.message;

    status = client->Check(taskId, result);
    EXPECT_EQ(status.code, StatusCode::TASK_NOT_FOUND);
}

TEST(AsuClientImplTest, Task_CheckKeepsEntryStatusInOriginalOrderAcrossAsus)
{
    auto state = std::make_shared<TestState>();
    state->checkEntryStatus[10] = {Status::OK()};
    state->checkEntryStatus[20] = {Status::Error(StatusCode::IO_ERROR, "entry on asu 20")};
    state->checkEntryStatus[30] = {Status::Error(StatusCode::NOT_FOUND, "entry on asu 30")};
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20, 30})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k25", {}},
            KVBuffer{"k05", {}},
            KVBuffer{"k15", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Check(taskId, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ASSERT_EQ(result.entryStatus.size(), std::size_t{3});
    EXPECT_EQ(result.entryStatus[0].code, StatusCode::NOT_FOUND);
    EXPECT_EQ(result.entryStatus[1].code, StatusCode::OK);
    EXPECT_EQ(result.entryStatus[2].code, StatusCode::IO_ERROR);
}

TEST(AsuClientImplTest, Task_LoadKeepsEntryStatusInOriginalOrderAcrossAsus)
{
    auto state = std::make_shared<TestState>();
    state->checkEntryStatus[10] = {Status::OK()};
    state->checkEntryStatus[20] = {Status::Error(StatusCode::IO_ERROR, "load entry on asu 20")};
    state->checkEntryStatus[30] = {Status::Error(StatusCode::NOT_FOUND, "load entry on asu 30")};
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20, 30})).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->LoadAsync(
        {
            KVBuffer{"k25", {}},
            KVBuffer{"k05", {}},
            KVBuffer{"k15", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Check(taskId, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ASSERT_EQ(result.entryStatus.size(), std::size_t{3});
    EXPECT_EQ(result.entryStatus[0].code, StatusCode::NOT_FOUND);
    EXPECT_EQ(result.entryStatus[1].code, StatusCode::OK);
    EXPECT_EQ(result.entryStatus[2].code, StatusCode::IO_ERROR);
}

TEST(AsuClientImplTest, Task_DeleteKeepsEntryStatusInOriginalOrderAcrossAsus)
{
    auto state = std::make_shared<TestState>();
    state->checkEntryStatus[10] = {Status::OK()};
    state->checkEntryStatus[20] = {Status::Error(StatusCode::IO_ERROR, "delete entry on asu 20")};
    state->checkEntryStatus[30] = {Status::Error(StatusCode::NOT_FOUND, "delete entry on asu 30")};
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20, 30})).ok());

    TaskId taskId = kInvalidTaskId;
    auto status = client->DeleteAsync({"k25", "k05", "k15"}, taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Check(taskId, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ASSERT_EQ(result.entryStatus.size(), std::size_t{3});
    EXPECT_EQ(result.entryStatus[0].code, StatusCode::NOT_FOUND);
    EXPECT_EQ(result.entryStatus[1].code, StatusCode::OK);
    EXPECT_EQ(result.entryStatus[2].code, StatusCode::IO_ERROR);
}

TEST(AsuClientImplTest, SnapshotRefresh_ReusesExistingTransportAndBindsResourcesToAddedAsu)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(std::vector<std::vector<AsuId>>{
        {10},
        {10, 20}
    });
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    std::vector<RegisterResult> results;
    auto status = client->RegisterRegions({MemoryRegion{}}, results);
    ASSERT_TRUE(status.ok()) << status.message;
    state->failFirstQuery = true;

    QueryResult result;
    status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    ASSERT_TRUE(client->Shutdown().ok());
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
    EXPECT_EQ(state->bindCalls, std::vector<AsuId>({20}));
}

TEST(AsuClientImplTest,
     SnapshotRefresh_RemovedAsuStopsReceivingNewRequestsButExistingTaskCanComplete)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{
            {10, 20},
            {10}
    },
        std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync(
        {
            KVBuffer{"k15", {}}
    },
        taskId);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_EQ(state->storeCalls, std::vector<AsuId>({20}));

    state->failFirstQuery = true;
    QueryResult queryResult;
    status = client->Query({"k05"}, QueryOptions{}, queryResult);
    ASSERT_EQ(status.code, StatusCode::CONNECTION_ERROR);
    ASSERT_TRUE(WaitForFetchCount(std::static_pointer_cast<FakeViewServer>(config.viewServer), 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TaskResult taskResult;
    status = client->Check(taskId, taskResult);
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->checkCalls, std::vector<AsuId>({20}));

    status = client->StoreAsync(
        {
            KVBuffer{"k15", {}}
    },
        taskId);
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->storeCalls, std::vector<AsuId>({20, 10}));
}

TEST(AsuClientImplTest, Hash_RingHashModeBuildsAndRoutes)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20, 30});
    config.hashTable.type = HashTableType::RING_HASH;
    config.hashTable.ringHash.virtualNodeCount = 1;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05", "k15", "k25", "k35"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({0, 1, 1, 0}));
    ExpectSameAsuSet(state->queryCalls, {10, 20, 30});
}

TEST(AsuClientImplTest, Hash_Crc32IEEERingHashDistributionIsBalanced)
{
    constexpr std::size_t kAsuCount = 16;
    constexpr std::size_t kKeyCount = 20000;
    constexpr double kMaxSkewRatio = 0.3;

    auto state = std::make_shared<TestState>();
    auto asuIds = MakeAsuIds(kAsuCount);
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeDistributionConfig(asuIds, HashTableType::RING_HASH)).ok());

    QueryResult result;
    auto keys = MakeKeys(kKeyCount);
    auto status = client->Query(keys, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ExpectBalancedDistribution(state->queryKeyCounts, asuIds, kKeyCount, kMaxSkewRatio);
}

TEST(AsuClientImplTest, Hash_StableHashRingHashDistributionIsBalanced)
{
    constexpr std::size_t kAsuCount = 16;
    constexpr std::size_t kKeyCount = 20000;
    constexpr double kMaxSkewRatio = 0.3;

    auto state = std::make_shared<TestState>();
    auto asuIds = MakeAsuIds(kAsuCount);
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(
        client->Init(MakeDistributionConfig(asuIds, HashTableType::RING_HASH, StableHash)).ok());

    QueryResult result;
    auto keys = MakeKeys(kKeyCount);
    auto status = client->Query(keys, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ExpectBalancedDistribution(state->queryKeyCounts, asuIds, kKeyCount, kMaxSkewRatio);
}

TEST(AsuClientImplTest, Hash_MaglevModeBuildsAndRoutes)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20, 30});
    config.hashTable.type = HashTableType::MAGLEV;
    config.hashTable.maglev.tableSize = 7;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05", "k15", "k25", "k35"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({0, 1, 1, 0}));
    ExpectSameAsuSet(state->queryCalls, {10, 20, 30});
}

TEST(AsuClientImplTest, Hash_Crc32IEEEMaglevDistributionIsBalanced)
{
    constexpr std::size_t kAsuCount = 16;
    constexpr std::size_t kKeyCount = 20000;
    constexpr double kMaxSkewRatio = 0.3;

    auto state = std::make_shared<TestState>();
    auto asuIds = MakeAsuIds(kAsuCount);
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeDistributionConfig(asuIds, HashTableType::MAGLEV)).ok());

    QueryResult result;
    auto keys = MakeKeys(kKeyCount);
    auto status = client->Query(keys, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ExpectBalancedDistribution(state->queryKeyCounts, asuIds, kKeyCount, kMaxSkewRatio);
}

TEST(AsuClientImplTest, Hash_StableHashMaglevDistributionIsBalanced)
{
    constexpr std::size_t kAsuCount = 16;
    constexpr std::size_t kKeyCount = 20000;
    constexpr double kMaxSkewRatio = 0.3;

    auto state = std::make_shared<TestState>();
    auto asuIds = MakeAsuIds(kAsuCount);
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(
        client->Init(MakeDistributionConfig(asuIds, HashTableType::MAGLEV, StableHash)).ok());

    QueryResult result;
    auto keys = MakeKeys(kKeyCount);
    auto status = client->Query(keys, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ExpectBalancedDistribution(state->queryKeyCounts, asuIds, kKeyCount, kMaxSkewRatio);
}

TEST(AsuClientImplTest, Hash_ContiguousBlockAffinityRoutesKKeysTogether)
{
    constexpr std::uint64_t kContiguousBlockCount = 3;
    constexpr std::size_t kKeyCount = 12;

    auto state = std::make_shared<TestState>();
    auto config =
        MakeDistributionConfig(MakeAsuIds(8), HashTableType::CONTIGUOUS_BLOCK_AFFINITY, StableHash);
    config.hashTable.contiguousBlockAffinity.blockCount = kContiguousBlockCount;
    config.hashTable.contiguousBlockAffinity.dynamicAdjustEnabled = true;

    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto keys = MakeKeys(kKeyCount);
    auto status = client->Query(keys, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    std::unordered_map<CacheKey, AsuId> routes;
    for (const auto& item : state->queryKeys) {
        for (const auto& key : item.second) { routes.emplace(key, item.first); }
    }

    for (std::size_t begin = 0; begin < keys.size(); begin += kContiguousBlockCount) {
        auto routeIter = routes.find(keys[begin]);
        ASSERT_NE(routeIter, routes.end());
        const auto groupAsuId = routeIter->second;
        const auto end = std::min<std::size_t>(keys.size(), begin + kContiguousBlockCount);
        for (std::size_t index = begin; index < end; ++index) {
            auto iter = routes.find(keys[index]);
            ASSERT_NE(iter, routes.end());
            EXPECT_EQ(iter->second, groupAsuId) << "key=" << keys[index];
        }
    }
}

TEST(AsuClientImplTest, Hash_ContiguousBlockAffinityUsesConfiguredFullSpreadType)
{
    constexpr std::uint64_t kContiguousBlockCount = 4;
    constexpr std::size_t kKeyCount = 16;

    auto asuIds = MakeAsuIds(8);
    auto state = std::make_shared<TestState>();
    auto config =
        MakeDistributionConfig(asuIds, HashTableType::CONTIGUOUS_BLOCK_AFFINITY, StableHash);
    config.hashTable.contiguousBlockAffinity.blockCount = kContiguousBlockCount;
    config.hashTable.contiguousBlockAffinity.fullSpreadType = HashTableType::MAGLEV_FULL_SPREAD;

    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto keys = MakeKeys(kKeyCount);
    auto status = client->Query(keys, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    std::unordered_map<CacheKey, AsuId> routes;
    for (const auto& item : state->queryKeys) {
        for (const auto& key : item.second) { routes.emplace(key, item.first); }
    }

    std::vector<UC::KV::NodeId> nodeIds(asuIds.begin(), asuIds.end());
    auto maglevConfig = config.hashTable;
    maglevConfig.type = HashTableType::MAGLEV_FULL_SPREAD;
    auto maglevRouter = UC::KV::CreateRouter(nodeIds, StableHash, maglevConfig);

    for (std::size_t begin = 0; begin < keys.size(); begin += kContiguousBlockCount) {
        const auto expectedRoute = maglevRouter->RouteKeys({keys[begin]});
        ASSERT_EQ(expectedRoute.size(), std::size_t{1});
        const auto expectedAsuId = expectedRoute.begin()->first;
        const auto end = std::min<std::size_t>(keys.size(), begin + kContiguousBlockCount);
        for (std::size_t index = begin; index < end; ++index) {
            auto iter = routes.find(keys[index]);
            ASSERT_NE(iter, routes.end());
            EXPECT_EQ(iter->second, expectedAsuId) << "key=" << keys[index];
        }
    }
}

TEST(AsuClientImplTest, Hash_ContiguousBlockAffinityRejectsNonFullSpreadType)
{
    auto state = std::make_shared<TestState>();
    auto config =
        MakeDistributionConfig(MakeAsuIds(8), HashTableType::CONTIGUOUS_BLOCK_AFFINITY, StableHash);
    config.hashTable.contiguousBlockAffinity.blockCount = 2;
    config.hashTable.contiguousBlockAffinity.fullSpreadType = HashTableType::BATCH_TOPK_AFFINITY;

    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query(MakeKeys(8), QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_TRUE(state->queryCalls.empty());
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>(8, 0));
}

TEST(AsuClientImplTest, Hash_BatchTopKAffinityLimitsTouchedAsus)
{
    constexpr std::size_t kAsuCount = 16;
    constexpr std::size_t kKeyCount = 100;
    constexpr std::size_t kTopK = 3;

    auto state = std::make_shared<TestState>();
    auto config = MakeDistributionConfig(MakeAsuIds(kAsuCount), HashTableType::BATCH_TOPK_AFFINITY,
                                         StableHash);
    config.hashTable.batchTopKAffinity.topK = kTopK;
    config.hashTable.batchTopKAffinity.dynamicAdjustEnabled = true;

    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query(MakeKeys(kKeyCount), QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_LE(state->queryKeyCounts.size(), kTopK);
    std::size_t routedKeyCount = 0;
    for (const auto& item : state->queryKeyCounts) { routedKeyCount += item.second; }
    EXPECT_EQ(routedKeyCount, kKeyCount);
}

TEST(AsuClientImplTest, Hash_RingHashMigrationRatioIsBoundedWhenAsuIsAdded)
{
    constexpr std::size_t kOldAsuCount = 16;
    constexpr std::size_t kKeyCount = 20000;
    constexpr double kMaxMigrationRatio = 0.15;

    auto oldAsuIds = MakeAsuIds(kOldAsuCount);
    auto newAsuIds = MakeAsuIds(kOldAsuCount + 1);
    auto keys = MakeKeys(kKeyCount);

    auto oldRoutes = CaptureKeyRoutes(oldAsuIds, HashTableType::RING_HASH, StableHash, keys);
    auto newRoutes = CaptureKeyRoutes(newAsuIds, HashTableType::RING_HASH, StableHash, keys);
    auto migrationRatio = CalculateMigrationRatio(oldRoutes, newRoutes);

    std::cout << "RingHash migration ratio after adding one ASU: " << migrationRatio << std::endl;
    EXPECT_LE(migrationRatio, kMaxMigrationRatio);
}

TEST(AsuClientImplTest, Hash_MaglevMigrationRatioIsBoundedWhenAsuIsAdded)
{
    constexpr std::size_t kOldAsuCount = 16;
    constexpr std::size_t kKeyCount = 20000;
    constexpr double kMaxMigrationRatio = 0.15;

    auto oldAsuIds = MakeAsuIds(kOldAsuCount);
    auto newAsuIds = MakeAsuIds(kOldAsuCount + 1);
    auto keys = MakeKeys(kKeyCount);

    auto oldRoutes = CaptureKeyRoutes(oldAsuIds, HashTableType::MAGLEV, StableHash, keys);
    auto newRoutes = CaptureKeyRoutes(newAsuIds, HashTableType::MAGLEV, StableHash, keys);
    auto migrationRatio = CalculateMigrationRatio(oldRoutes, newRoutes);

    std::cout << "Maglev migration ratio after adding one ASU: " << migrationRatio << std::endl;
    EXPECT_LE(migrationRatio, kMaxMigrationRatio);
}

}  // namespace UC::ASU
