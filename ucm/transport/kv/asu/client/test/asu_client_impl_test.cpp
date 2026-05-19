#include "asu_client/asu_client.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
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
    std::unordered_map<AsuId, std::vector<Status>> checkEntryStatus;
    std::unordered_map<AsuId, Status> checkResultStatus;
    std::vector<AsuId> registerCalls;
    std::vector<AsuId> bindCalls;
    std::vector<AsuId> unregisterCalls;
    std::vector<AsuId> queryCalls;
    std::unordered_map<AsuId, std::size_t> queryKeyCounts;
    std::vector<AsuId> storeCalls;
    std::vector<AsuId> deleteCalls;
    std::vector<AsuId> checkCalls;
    std::vector<AsuId> waitCalls;
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

    Status Query(const std::vector<CacheKey>& keys, const QueryOptions&, QueryResult& result) override
    {
        if (!initialized_) { return NotInitialized(); }
        if (state_->failFirstQuery && !state_->firstQueryFailed) {
            state_->firstQueryFailed = true;
            return Status::Error(state_->firstQueryFailureCode, state_->firstQueryFailureMessage);
        }

        state_->queryCalls.emplace_back(config_.asu_id);
        state_->queryKeyCounts[config_.asu_id] += keys.size();
        result.exists.clear();
        result.exists.reserve(keys.size());
        for (const auto& key : keys) {
            result.exists.emplace_back(key == "k15" || key == "k25");
        }
        result.prefix_hit_keys = 0;
        return Status::OK();
    }

    Status QueryAsync(const std::vector<CacheKey>&, const QueryOptions&, TaskId& taskId) override
    {
        taskId = 0;
        return Status::OK();
    }

    Status LoadAsync(const std::vector<KVBuffer>&, TaskId& taskId) override
    {
        taskId = 1000 + config_.asu_id;
        state_->childTaskIds[config_.asu_id] = taskId;
        return Status::OK();
    }

    Status StoreAsync(const std::vector<KVBuffer>&, TaskId& taskId) override
    {
        state_->storeCalls.emplace_back(config_.asu_id);
        taskId = 2000 + config_.asu_id;
        state_->childTaskIds[config_.asu_id] = taskId;
        return Status::OK();
    }

    Status DeleteAsync(const std::vector<CacheKey>&, TaskId& taskId) override
    {
        state_->deleteCalls.emplace_back(config_.asu_id);
        taskId = 3000 + config_.asu_id;
        state_->childTaskIds[config_.asu_id] = taskId;
        return Status::OK();
    }

    Status Cancel(TaskId) override { return Status::OK(); }

    Status Check(TaskId taskId, TaskResult& result) override
    {
        state_->checkCalls.emplace_back(config_.asu_id);
        auto statusIter = state_->checkResultStatus.find(config_.asu_id);
        result.status = statusIter == state_->checkResultStatus.end() ? Status::OK()
                                                                      : statusIter->second;
        auto entryIter = state_->checkEntryStatus.find(config_.asu_id);
        result.entry_status = entryIter == state_->checkEntryStatus.end()
                                  ? std::vector<Status>{result.status}
                                  : entryIter->second;
        result.query_result.reset();
        if (taskId == 0) {
            return Status::Error(StatusCode::TASK_NOT_FOUND, "fake task not found");
        }
        return Status::OK();
    }

    Status Wait(TaskId taskId, std::uint64_t, TaskResult& result) override
    {
        state_->waitCalls.emplace_back(config_.asu_id);
        return Check(taskId, result);
    }

    Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                           std::vector<RegisterResult>& results) override
    {
        state_->registerCalls.emplace_back(config_.asu_id);
        results.clear();
        for (std::size_t index = 0; index < regions.size(); ++index) {
            results.emplace_back(RegisterResult{Status::OK(), 500 + index});
        }
        return Status::OK();
    }

    Status BindRegisteredRegions(const std::vector<RegisteredMemory>& regions,
                                 std::vector<RegisterResult>& results) override
    {
        state_->bindCalls.emplace_back(config_.asu_id);
        results.clear();
        for (const auto& region : regions) {
            results.emplace_back(RegisterResult{Status::OK(), region.handle});
        }
        return Status::OK();
    }

    Status UnregisterRegions(const std::vector<MRHandle>&) override
    {
        state_->unregisterCalls.emplace_back(config_.asu_id);
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
        : views_(std::move(views)),
          epochs_(std::move(epochs))
    {
    }

    Status GetGlobalView(GlobalView& view) override
    {
        if (failFetchAt_ != 0 && fetchCount_ + 1 == failFetchAt_) {
            ++fetchCount_;
            return Status::Error(StatusCode::IO_ERROR, "fake view fetch failed");
        }

        auto index = fetchCount_;
        if (index >= views_.size()) { index = views_.size() - 1; }
        ++fetchCount_;

        view = GlobalView{};
        for (auto asuId : views_[index]) {
            view.asuMap.emplace(asuId, AsuInfo{asuId});
        }
        view.viewEpoch = index < epochs_.size() ? epochs_[index] : fetchCount_;
        return Status::OK();
    }

    void FailFetchAt(std::size_t fetchCount) { failFetchAt_ = fetchCount; }
    std::size_t FetchCount() const { return fetchCount_; }

private:
    std::size_t fetchCount_{0};
    std::size_t failFetchAt_{0};
    std::vector<std::vector<AsuId>> views_;
    std::vector<std::uint64_t> epochs_;
};

AsuClientConfig MakeConfig(const std::vector<AsuId>& asuIds)
{
    AsuClientConfig config;
    for (auto asuId : asuIds) {
        TransportConfig transportConfig;
        transportConfig.asu_id = asuId;
        config.transportConfigs.emplace_back(std::move(transportConfig));
    }
    config.hashTable.virtualNodeCount = 1;
    config.hash = [](const std::string& key) -> std::uint64_t {
        static const std::unordered_map<std::string, std::uint64_t> values{
            {"vn-0#asu-10", 10},
            {"vn-0#asu-20", 20},
            {"vn-0#asu-30", 30},
            {"k05", 5},
            {"k15", 15},
            {"k25", 25},
            {"k35", 35},
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
    for (unsigned char ch : data) {
        crc = table[(crc ^ ch) & 0xFFU] ^ (crc >> 8U);
    }
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

std::uint64_t StableHash(const std::string& value)
{
    return SplitMix64(Fnv1a64(value));
}

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
    config.hashTable.virtualNodeCount = 256;
    config.hashTable.maglevTableSize = 65537;
    return config;
}

void ExpectBalancedDistribution(const std::unordered_map<AsuId, std::size_t>& keyCounts,
                                const std::vector<AsuId>& asuIds, std::size_t totalKeyCount,
                                double maxSkewRatio)
{
    ASSERT_EQ(keyCounts.size(), asuIds.size());

    const auto expectedCount = static_cast<double>(totalKeyCount) /
                               static_cast<double>(asuIds.size());
    for (auto asuId : asuIds) {
        auto iter = keyCounts.find(asuId);
        ASSERT_NE(iter, keyCounts.end());
        const auto diff = iter->second > expectedCount ? iter->second - expectedCount
                                                       : expectedCount - iter->second;
        EXPECT_LE(diff / expectedCount, maxSkewRatio) << "asuId=" << asuId
                                                      << " keyCount=" << iter->second
                                                      << " expected=" << expectedCount;
    }
}

TEST(AsuClientImplTest, QueryPerKeyKeepsOriginalOrder)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20})).ok());

    QueryResult result;
    auto status = client->Query({"k05", "k15", "k25"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({0, 1, 1}));
}

TEST(AsuClientImplTest, RefreshesAndRetriesQueryOnce)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20})).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({0}));
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
}

TEST(AsuClientImplTest, DoesNotRefreshOrRetryNonRefreshableQueryError)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    state->firstQueryFailureCode = StatusCode::IO_ERROR;
    state->firstQueryFailureMessage = "fake io error";
    auto viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{{10}, {10, 20}}, std::vector<std::uint64_t>{1, 2});
    auto config = MakeConfig({10, 20});
    config.viewServer = viewServer;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::IO_ERROR);
    EXPECT_EQ(viewServer->FetchCount(), std::size_t{1});
    EXPECT_EQ(state->createdTransports, std::uint32_t{1});
}

TEST(AsuClientImplTest, ReturnsRefreshFailureWhenRetryCannotFetchView)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    auto viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{{10}, {10, 20}}, std::vector<std::uint64_t>{1, 2});
    viewServer->FailFetchAt(2);
    auto config = MakeConfig({10, 20});
    config.viewServer = viewServer;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_EQ(status.code, StatusCode::IO_ERROR);
    EXPECT_NE(status.message.find("failed to refresh view before retrying query"),
              std::string::npos);
    EXPECT_EQ(state->createdTransports, std::uint32_t{1});
}

TEST(AsuClientImplTest, DoesNotPublishSameOrOlderViewEpoch)
{
    auto state = std::make_shared<TestState>();
    state->failFirstQuery = true;
    auto viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{{10}, {10, 20}, {10, 20}},
        std::vector<std::uint64_t>{5, 5, 4});
    auto config = MakeConfig({10, 20});
    config.viewServer = viewServer;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05"}, QueryOptions{}, result);
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->createdTransports, std::uint32_t{1});

    state->failFirstQuery = true;
    state->firstQueryFailed = false;
    status = client->Query({"k05"}, QueryOptions{}, result);
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->createdTransports, std::uint32_t{1});
}

TEST(AsuClientImplTest, RegisterRegionsRegistersFirstTransportAndBindsFollowers)
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

TEST(AsuClientImplTest, RegisterRegionsBindFailureIncludesAsuContext)
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

TEST(AsuClientImplTest, RemovesTaskAfterCompletion)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync({KVBuffer{"k05", {}}}, taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Check(taskId, result);
    ASSERT_TRUE(status.ok()) << status.message;

    status = client->Check(taskId, result);
    EXPECT_EQ(status.code, StatusCode::TASK_NOT_FOUND);
}

TEST(AsuClientImplTest, KeepsInProgressTaskUntilCompletion)
{
    auto state = std::make_shared<TestState>();
    state->checkResultStatus[10] = Status::Error(StatusCode::IN_PROGRESS, "fake in progress");
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync({KVBuffer{"k05", {}}}, taskId);
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

TEST(AsuClientImplTest, WaitRemovesTaskAfterCompletion)
{
    auto state = std::make_shared<TestState>();
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync({KVBuffer{"k05", {}}}, taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Wait(taskId, 10, result);
    ASSERT_TRUE(status.ok()) << status.message;

    status = client->Wait(taskId, 10, result);
    EXPECT_EQ(status.code, StatusCode::TASK_NOT_FOUND);
}

TEST(AsuClientImplTest, CheckKeepsEntryStatusInOriginalOrderAcrossAsus)
{
    auto state = std::make_shared<TestState>();
    state->checkEntryStatus[10] = {Status::OK()};
    state->checkEntryStatus[20] = {Status::Error(StatusCode::IO_ERROR, "entry on asu 20")};
    state->checkEntryStatus[30] = {Status::Error(StatusCode::NOT_FOUND, "entry on asu 30")};
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeConfig({10, 20, 30})).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync({KVBuffer{"k25", {}}, KVBuffer{"k05", {}},
                                      KVBuffer{"k15", {}}},
                                     taskId);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult result;
    status = client->Check(taskId, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ASSERT_EQ(result.entry_status.size(), std::size_t{3});
    EXPECT_EQ(result.entry_status[0].code, StatusCode::NOT_FOUND);
    EXPECT_EQ(result.entry_status[1].code, StatusCode::OK);
    EXPECT_EQ(result.entry_status[2].code, StatusCode::IO_ERROR);
}

TEST(AsuClientImplTest, RefreshViewReusesExistingTransportAndBindsResourcesToAddedAsu)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{{10}, {10, 20}});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    std::vector<RegisterResult> results;
    auto status = client->RegisterRegions({MemoryRegion{}}, results);
    ASSERT_TRUE(status.ok()) << status.message;
    state->failFirstQuery = true;

    QueryResult result;
    status = client->Query({"k05"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
    EXPECT_EQ(state->bindCalls, std::vector<AsuId>({20}));
}

TEST(AsuClientImplTest, RemovedAsuStopsReceivingNewRequestsButExistingTaskCanComplete)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{{10, 20}, {10}}, std::vector<std::uint64_t>{1, 2});
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    TaskId taskId = 0;
    auto status = client->StoreAsync({KVBuffer{"k15", {}}}, taskId);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_EQ(state->storeCalls, std::vector<AsuId>({20}));

    state->failFirstQuery = true;
    QueryResult queryResult;
    status = client->Query({"k05"}, QueryOptions{}, queryResult);
    ASSERT_TRUE(status.ok()) << status.message;

    TaskResult taskResult;
    status = client->Check(taskId, taskResult);
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->checkCalls, std::vector<AsuId>({20}));

    status = client->StoreAsync({KVBuffer{"k15", {}}}, taskId);
    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->storeCalls, std::vector<AsuId>({20, 10}));
}

TEST(AsuClientImplTest, MaglevModeBuildsAndRoutes)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20, 30});
    config.hashTable.type = HashTableType::MAGLEV;
    config.hashTable.maglevTableSize = 7;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05", "k15", "k25", "k35"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({0, 1, 1, 0}));
    EXPECT_FALSE(state->queryCalls.empty());
}

TEST(AsuClientImplTest, RingHashModeBuildsAndRoutes)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20, 30});
    config.hashTable.type = HashTableType::RING_HASH;
    config.hashTable.virtualNodeCount = 1;
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(config).ok());

    QueryResult result;
    auto status = client->Query({"k05", "k15", "k25", "k35"}, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(result.exists, std::vector<std::uint8_t>({0, 1, 1, 0}));
    EXPECT_EQ(state->queryCalls, std::vector<AsuId>({10, 20, 30}));
}

TEST(AsuClientImplTest, Crc32IEEERingHashDistributionIsBalanced)
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

TEST(AsuClientImplTest, Crc32IEEEMaglevDistributionIsBalanced)
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

TEST(AsuClientImplTest, StableHashRingHashDistributionIsBalanced)
{
    constexpr std::size_t kAsuCount = 16;
    constexpr std::size_t kKeyCount = 20000;
    constexpr double kMaxSkewRatio = 0.3;

    auto state = std::make_shared<TestState>();
    auto asuIds = MakeAsuIds(kAsuCount);
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeDistributionConfig(asuIds, HashTableType::RING_HASH, StableHash)).ok());

    QueryResult result;
    auto keys = MakeKeys(kKeyCount);
    auto status = client->Query(keys, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ExpectBalancedDistribution(state->queryKeyCounts, asuIds, kKeyCount, kMaxSkewRatio);
}

TEST(AsuClientImplTest, StableHashMaglevDistributionIsBalanced)
{
    constexpr std::size_t kAsuCount = 16;
    constexpr std::size_t kKeyCount = 20000;
    constexpr double kMaxSkewRatio = 0.3;

    auto state = std::make_shared<TestState>();
    auto asuIds = MakeAsuIds(kAsuCount);
    auto client = CreateAsuClient(MakeFactory(state));
    ASSERT_TRUE(client->Init(MakeDistributionConfig(asuIds, HashTableType::MAGLEV, StableHash)).ok());

    QueryResult result;
    auto keys = MakeKeys(kKeyCount);
    auto status = client->Query(keys, QueryOptions{}, result);

    EXPECT_TRUE(status.ok()) << status.message;
    ExpectBalancedDistribution(state->queryKeyCounts, asuIds, kKeyCount, kMaxSkewRatio);
}

TEST(AsuClientImplTest, UnregisterRemovesCachedResourceBeforeFutureAsuIsAdded)
{
    auto state = std::make_shared<TestState>();
    auto config = MakeConfig({10, 20});
    config.viewServer = std::make_shared<FakeViewServer>(
        std::vector<std::vector<AsuId>>{{10}, {10, 20}}, std::vector<std::uint64_t>{1, 2});
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

    EXPECT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(state->createdTransports, std::uint32_t{2});
    EXPECT_TRUE(state->bindCalls.empty());
}

}  // namespace UC::ASU
