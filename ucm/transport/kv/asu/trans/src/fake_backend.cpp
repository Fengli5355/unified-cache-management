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
#include "asu_transport/fake_backend.h"
#include <acl/acl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include "aicpu_trans_provider.h"
#include "kv_protocol.h"
#include "trans_provider.h"

namespace UC::ASU {
namespace {

constexpr std::uint16_t kCqeSuccess = 0x000;
constexpr std::uint16_t kCqeCheckResultBuffer = 0x732;
constexpr std::uint8_t kBatchEntryOk = 0x0;
constexpr std::uint8_t kBatchEntryKeyNotFound = 0x3;
constexpr std::uint8_t kDeleteEntryOk = 0x0;
constexpr std::uint8_t kDeleteEntryFailed = 0x1;
constexpr std::uint8_t kExistEntryNotExist = 0x0;
constexpr std::uint8_t kExistEntryExist = 0x1;

std::mutex g_fakeBackendMu;
FakeBackendConfig g_fakeBackendConfig;
bool g_fakeBackendEnabled = false;

Status SetUpAclRuntime(const FakeBackendConfig& config)
{
    auto ret = aclInit(nullptr);
    if (ret != ACL_SUCCESS && ret != ACL_ERROR_REPEAT_INITIALIZE) {
        return Status::Error(StatusCode::INTERNAL_ERROR,
                             "ASU fake backend aclInit failed: " + std::to_string(ret));
    }

    const auto deviceId = config.deviceId < 0 ? 0 : config.deviceId;
    ret = aclrtSetDevice(deviceId);
    if (ret != ACL_SUCCESS) {
        return Status::Error(StatusCode::INTERNAL_ERROR,
                             "ASU fake backend aclrtSetDevice failed: device_id=" +
                                 std::to_string(deviceId) + " ret=" + std::to_string(ret));
    }
    return Status::OK();
}

std::filesystem::path StoreRoot(const FakeBackendConfig& config)
{
    if (!config.storePath.empty()) { return config.storePath; }
    return "./asu-fake-backend-store";
}

std::uint64_t ReadU64(std::uint32_t low, std::uint32_t high)
{
    return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
}

std::uint32_t RequestCid(const std::uint32_t* request) { return request[0] >> 16; }

AsuId RequestAsuId(const std::uint32_t* request) { return request[1]; }

KvOpcode RequestOpcode(const std::uint32_t* request)
{
    return static_cast<KvOpcode>(request[0] & 0xFF);
}

std::string ReadKey(const std::uint32_t* data)
{
    char key[17] = {};
    std::memcpy(key, data, 16);
    const auto keyLen = std::find(key, key + 16, '\0') - key;
    return std::string(key, static_cast<std::size_t>(keyLen));
}

std::string KeyFileName(const std::string& key)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : key) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash << ".bin";
    return stream.str();
}

std::filesystem::path AsuRoot(const FakeBackendConfig& config, AsuId asuId)
{
    return StoreRoot(config) / ("asu-" + std::to_string(asuId));
}

std::filesystem::path KeyPath(const FakeBackendConfig& config, AsuId asuId, const std::string& key)
{
    return AsuRoot(config, asuId) / KeyFileName(key);
}

bool StoreBytes(const FakeBackendConfig& config, AsuId asuId, const std::string& key,
                std::uint64_t addr, std::uint32_t length)
{
    std::filesystem::create_directories(AsuRoot(config, asuId));
    std::ofstream output(KeyPath(config, asuId, key), std::ios::binary | std::ios::trunc);
    if (!output) { return false; }
    output.write(reinterpret_cast<const char*>(addr), length);
    return output.good();
}

bool LoadBytes(const FakeBackendConfig& config, AsuId asuId, const std::string& key,
               std::uint64_t addr, std::uint32_t length)
{
    std::ifstream input(KeyPath(config, asuId, key), std::ios::binary);
    if (!input) { return false; }
    input.read(reinterpret_cast<char*>(addr), length);
    const auto readCount = input.gcount();
    if (readCount < static_cast<std::streamsize>(length)) {
        std::memset(reinterpret_cast<char*>(addr) + readCount, 0,
                    length - static_cast<std::uint32_t>(readCount));
    }
    return true;
}

bool DeleteKey(const FakeBackendConfig& config, AsuId asuId, const std::string& key)
{
    std::error_code errorCode;
    std::filesystem::remove(KeyPath(config, asuId, key), errorCode);
    return !errorCode;
}

bool ExistsKey(const FakeBackendConfig& config, AsuId asuId, const std::string& key)
{
    std::error_code errorCode;
    return std::filesystem::exists(KeyPath(config, asuId, key), errorCode);
}

void PackCqeHeader(std::uint32_t* flagBuffer, std::uint16_t cid, std::uint16_t status)
{
    flagBuffer[0] = 0;
    flagBuffer[1] = 0;
    flagBuffer[2] = 0;
    flagBuffer[3] = static_cast<std::uint32_t>(cid) | (static_cast<std::uint32_t>(status) << 17);
}

void PackResultBuffer4Bit(std::uint32_t* resultData, const std::vector<std::uint8_t>& results)
{
    const auto dwordCount = (results.size() + 7) / 8;
    std::fill(resultData, resultData + dwordCount, 0);
    for (std::size_t index = 0; index < results.size(); ++index) {
        resultData[index / 8] |= static_cast<std::uint32_t>(results[index] & 0xF)
                                 << ((index % 8) * 4);
    }
}

void PackResultBuffer1Bit(std::uint32_t* resultData, const std::vector<std::uint8_t>& results)
{
    const auto dwordCount = (results.size() + 31) / 32;
    std::fill(resultData, resultData + dwordCount, 0);
    for (std::size_t index = 0; index < results.size(); ++index) {
        resultData[index / 32] |= static_cast<std::uint32_t>(results[index] & 0x1) << (index % 32);
    }
}

struct BatchEntry {
    std::string key;
    std::uint64_t bufferAddr{0};
    std::uint32_t length{0};
};

std::vector<BatchEntry> ReadBatchEntries(const std::uint32_t* request, std::uint16_t batchNumber)
{
    std::vector<BatchEntry> entries;
    entries.reserve(batchNumber);
    for (std::uint16_t index = 0; index < batchNumber; ++index) {
        const auto* entry = request + kSqeDwordCount + index * kBatchEntryDwordCount;
        BatchEntry parsed;
        parsed.key = ReadKey(entry + 1);
        parsed.bufferAddr = ReadU64(entry[5], entry[6]);
        parsed.length = entry[7] & 0xFFFFFF;
        entries.emplace_back(std::move(parsed));
    }
    return entries;
}

std::vector<std::string> ReadKeyEntries(const std::uint32_t* request, std::uint16_t batchNumber)
{
    std::vector<std::string> keys;
    keys.reserve(batchNumber);
    for (std::uint16_t index = 0; index < batchNumber; ++index) {
        const auto* entry = request + kSqeDwordCount + index * kKeyEntryDwordCount;
        keys.emplace_back(ReadKey(entry));
    }
    return keys;
}

Status CompleteBatchStore(const FakeBackendConfig& config, AsuId asuId,
                          const std::uint32_t* request)
{
    const auto cid = static_cast<std::uint16_t>(RequestCid(request));
    const auto responseBufferAddr = ReadU64(request[3], request[4]);
    const auto batchNumber = static_cast<std::uint16_t>(request[10] & 0xFFFF);
    auto* flagBuffer = reinterpret_cast<std::uint32_t*>(responseBufferAddr);
    std::vector<std::uint8_t> results(batchNumber, kBatchEntryOk);

    const auto entries = ReadBatchEntries(request, batchNumber);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (!StoreBytes(config, asuId, entry.key, entry.bufferAddr, entry.length)) {
            results[index] = kBatchEntryKeyNotFound;
        }
    }

    const auto allOk = std::all_of(results.begin(), results.end(),
                                   [](std::uint8_t result) { return result == kBatchEntryOk; });
    const auto cqeStatus = allOk ? kCqeSuccess : kCqeCheckResultBuffer;
    PackCqeHeader(flagBuffer, cid, cqeStatus);
    if (!allOk) { PackResultBuffer4Bit(flagBuffer + kCqeDwordCount, results); }
    return Status::OK();
}

Status CompleteBatchRetrieve(const FakeBackendConfig& config, AsuId asuId,
                             const std::uint32_t* request)
{
    const auto cid = static_cast<std::uint16_t>(RequestCid(request));
    const auto responseBufferAddr = ReadU64(request[3], request[4]);
    const auto batchNumber = static_cast<std::uint16_t>(request[10] & 0xFFFF);
    auto* flagBuffer = reinterpret_cast<std::uint32_t*>(responseBufferAddr);
    std::vector<std::uint8_t> results(batchNumber, kBatchEntryOk);

    const auto entries = ReadBatchEntries(request, batchNumber);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        if (!LoadBytes(config, asuId, entry.key, entry.bufferAddr, entry.length)) {
            results[index] = kBatchEntryKeyNotFound;
        }
    }

    const auto allOk = std::all_of(results.begin(), results.end(),
                                   [](std::uint8_t result) { return result == kBatchEntryOk; });
    const auto cqeStatus = allOk ? kCqeSuccess : kCqeCheckResultBuffer;
    PackCqeHeader(flagBuffer, cid, cqeStatus);
    if (!allOk) { PackResultBuffer4Bit(flagBuffer + kCqeDwordCount, results); }
    return Status::OK();
}

Status CompleteDelete(const FakeBackendConfig& config, AsuId asuId, const std::uint32_t* request)
{
    const auto cid = static_cast<std::uint16_t>(RequestCid(request));
    const auto responseBufferAddr = ReadU64(request[3], request[4]);
    const auto batchNumber = static_cast<std::uint16_t>(request[5] & 0xFFFF);
    auto* flagBuffer = reinterpret_cast<std::uint32_t*>(responseBufferAddr);
    std::vector<std::uint8_t> results(batchNumber, kDeleteEntryOk);

    const auto keys = ReadKeyEntries(request, batchNumber);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (!DeleteKey(config, asuId, keys[index])) { results[index] = kDeleteEntryFailed; }
    }

    const auto allOk = std::all_of(results.begin(), results.end(),
                                   [](std::uint8_t result) { return result == kDeleteEntryOk; });
    const auto cqeStatus = allOk ? kCqeSuccess : kCqeCheckResultBuffer;
    PackCqeHeader(flagBuffer, cid, cqeStatus);
    if (!allOk) { PackResultBuffer1Bit(flagBuffer + kCqeDwordCount, results); }
    return Status::OK();
}

Status CompleteExist(const FakeBackendConfig& config, AsuId asuId, const std::uint32_t* request)
{
    const auto cid = static_cast<std::uint16_t>(RequestCid(request));
    const auto responseBufferAddr = ReadU64(request[3], request[4]);
    const auto batchNumber = static_cast<std::uint16_t>(request[5] & 0xFFFF);
    auto* flagBuffer = reinterpret_cast<std::uint32_t*>(responseBufferAddr);
    std::vector<std::uint8_t> results(batchNumber, kExistEntryNotExist);
    std::uint16_t existingKeyNumber = 0;

    const auto keys = ReadKeyEntries(request, batchNumber);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (ExistsKey(config, asuId, keys[index])) {
            results[index] = kExistEntryExist;
            ++existingKeyNumber;
        }
    }

    const auto allExist = existingKeyNumber == batchNumber;
    const auto cqeStatus = allExist ? kCqeSuccess : kCqeCheckResultBuffer;
    PackCqeHeader(flagBuffer, cid, cqeStatus);
    if (!allExist) { PackResultBuffer1Bit(flagBuffer + kCqeDwordCount, results); }
    return Status::OK();
}

Status CompleteFakeBackendRequest(FakeBackendConfig config, const void* sendBuffer,
                                  std::uint64_t len)
{
    if (config.latencyMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config.latencyMs));
    }

    if (sendBuffer == nullptr || len < sizeof(std::uint32_t)) {
        return Status::Error(StatusCode::INVALID_ARGUMENT, "fake backend send buffer is empty");
    }

    const auto* request = reinterpret_cast<const std::uint32_t*>(sendBuffer);
    const auto asuId = RequestAsuId(request);
    switch (RequestOpcode(request)) {
        case KvOpcode::BatchStore: return CompleteBatchStore(config, asuId, request);
        case KvOpcode::BatchRetrieve: return CompleteBatchRetrieve(config, asuId, request);
        case KvOpcode::Delete: return CompleteDelete(config, asuId, request);
        case KvOpcode::Exist: return CompleteExist(config, asuId, request);
        case KvOpcode::KeepAlive: {
            auto* flagBuffer = reinterpret_cast<std::uint32_t*>(ReadU64(request[3], request[4]));
            PackCqeHeader(flagBuffer, static_cast<std::uint16_t>(RequestCid(request)), kCqeSuccess);
            return Status::OK();
        }
        default:
            return Status::Error(StatusCode::UNSUPPORTED,
                                 "fake backend only supports batch ASU operations");
    }
}

FakeBackendConfig GetFakeBackendConfig(bool& enabled)
{
    std::lock_guard<std::mutex> lock(g_fakeBackendMu);
    enabled = g_fakeBackendEnabled;
    return g_fakeBackendConfig;
}

std::vector<Status> FakeBackendSend(const std::vector<TransProvider::SendIoBatch>& ioBatches,
                                    std::uint32_t kernelCount, std::uint32_t quietCount)
{
    (void)kernelCount;
    (void)quietCount;

    bool enabled = false;
    const auto config = GetFakeBackendConfig(enabled);
    if (!enabled) {
        return std::vector<Status>(
            ioBatches.size(),
            Status::Error(StatusCode::UNSUPPORTED, "ASU fake backend Send is not enabled"));
    }

    std::vector<Status> statuses;
    statuses.reserve(ioBatches.size());
    for (const auto& ioBatch : ioBatches) {
        statuses.emplace_back(CompleteFakeBackendRequest(config, ioBatch.sendBuffer, ioBatch.len));
    }
    return statuses;
}

}  // namespace

Status EnableFakeBackend(FakeBackendConfig config)
{
    if (config.storePath.empty()) { config.storePath = "./asu-fake-backend-store"; }
    auto status = SetUpAclRuntime(config);
    if (!status.ok()) { return status; }
    {
        std::lock_guard<std::mutex> lock(g_fakeBackendMu);
        g_fakeBackendConfig = std::move(config);
        g_fakeBackendEnabled = true;
    }
    SetAICPUTransProviderSendHook(&FakeBackendSend);
    return Status::OK();
}

void DisableFakeBackend()
{
    SetAICPUTransProviderSendHook(nullptr);
    {
        std::lock_guard<std::mutex> lock(g_fakeBackendMu);
        g_fakeBackendConfig = FakeBackendConfig{};
        g_fakeBackendEnabled = false;
    }
}

void PatchFakeBackendTransportConfig(TransportConfig& config)
{
    config.attrs.try_emplace("kernel_count", "1");
    config.attrs.try_emplace("quiet_count", "1");
    config.attrs["kv_ns_id"] = std::to_string(config.asuId);
    config.attrs.try_emplace("dtype", "0");
    config.attrs.try_emplace("dspec", "0");
    config.attrs.try_emplace("lr", "false");
    config.attrs["sc"] = "true";
    if (config.endpoints.empty()) {
        AsuEndpoint endpoint;
        endpoint.ip = "fake_backend";
        endpoint.port = 19001;
        endpoint.protocol = Protocol::TCP;
        config.endpoints.emplace_back(std::move(endpoint));
    }
}

}  // namespace UC::ASU
