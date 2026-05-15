#ifndef UNIFIEDCACHE_ASUSTORE_TRANSPORT_CLIENT_H
#define UNIFIEDCACHE_ASUSTORE_TRANSPORT_CLIENT_H

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace UC::AsuStore {

using TaskId = uint64_t;
using MRHandle = uint64_t;
using CacheKey = std::string;

static constexpr uint64_t U64Max = std::numeric_limits<uint64_t>::max();

struct KVBuffer {
    CacheKey key;
    uint64_t hbmAddr{0};
    uint64_t size{0};
    uint64_t entryIdx{0};
    uint64_t asuId{U64Max};
};

enum class StatusCode {
    OK = 0,
    INVALID_ARGUMENT,
    NOT_INITIALIZED,
    TIMEOUT,
    NOT_FOUND,
    PARTIAL_FAILED,
    CONNECTION_ERROR,
    IO_ERROR,
    BUFFER_NOT_REGISTERED,
    TASK_NOT_FOUND,
    INTERNAL_ERROR,
};
struct Status {
    StatusCode code{StatusCode::OK};
    std::string message;
};
enum class QueryMode {
    PER_KEY = 0,
    PREFIX = 1,
};
struct QueryResult {
    std::vector<uint8_t> exists;
    uint32_t prefixHitKeys{0};
};

enum class MemoryType {
    HOST = 0,
    HOST_PINNED = 1,
    ASCEND_DEVICE = 2,
};
struct MemorySpan {
    MemoryType memoryType{MemoryType::HOST};
    uint64_t addr{0};
    uint64_t size{0};
    int32_t deviceId{-1};
    int32_t numaNode{-1};
};
using MemoryRegion = MemorySpan;

struct Buffer {
    MemorySpan span;
    MRHandle handle{0};
};

struct ClientConfig {
    std::string clientId;
    std::vector<std::string> viewServiceAddrs;  // ip:port
    uint64_t connectTimeoutMs{1000};
    uint64_t ioTimeoutMs{1000};
    uint64_t queryTimeoutMs{300};

    uint32_t ioThreadNum{0};
    bool enableCpuAffinity{false};
    std::string cpuAffinityPolicy;
    std::unordered_map<std::string, std::string> attrs;  // resv
};

struct QueryOptions {
    QueryMode mode{QueryMode::PER_KEY};
};

struct RegisterResult {
    Status status;
    MRHandle handle{0};
};

struct TaskResult {
    Status status;
    std::vector<Status> entryStatus;
};

class TransportClient {
public:
    virtual ~TransportClient() = default;

    virtual Status Init(const ClientConfig& config) = 0;
    virtual Status Shutdown() = 0;

    virtual Status Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                         QueryResult& resp) = 0;
    virtual Status Load(const std::vector<KVBuffer>& entries, TaskId& taskId) = 0;
    virtual Status Store(const std::vector<KVBuffer>& entries, TaskId& taskId) = 0;
    virtual Status Delete(const std::vector<CacheKey>& keys) = 0;

    virtual Status LoadSingle(const KVBuffer& entry, TaskId& taskId) = 0;
    virtual Status StoreSingle(const KVBuffer& entry, TaskId& taskId) = 0;

    virtual Status Wait(TaskId taskId, uint64_t timeoutMs, TaskResult& result) = 0;
    virtual Status Check(TaskId taskId, TaskResult& result) = 0;

    virtual Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                                   std::vector<RegisterResult>& result) = 0;
    virtual Status UnRegisterRegions(const std::vector<MRHandle>& handles) = 0;
};

}  // namespace UC::AsuStore

#endif
