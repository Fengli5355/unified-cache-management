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
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <malloc.h>
#include <sstream>
#include <string>
#include <thread>
#include "asu_client/asu_client.h"

namespace UC::ASU {
namespace {

constexpr const char* kProbeConfigEnv = "ASU_CLIENT_MEMORY_PROBE_CONFIG";
constexpr auto kStabilizationDelay = std::chrono::seconds(1);

struct ProcessMemorySnapshot {
    std::uint64_t heapInUseBytes{0};
    std::uint64_t heapMappedBytes{0};
    std::uint64_t rssKiB{0};
    std::uint64_t pssKiB{0};
    std::uint64_t privateKiB{0};
};

ProcessMemorySnapshot TakeProcessMemorySnapshot()
{
    ProcessMemorySnapshot snapshot;
    const auto heap = mallinfo2();
    snapshot.heapInUseBytes = heap.uordblks;
    snapshot.heapMappedBytes = heap.hblkhd;

    std::ifstream file("/proc/self/smaps_rollup");
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream stream(line);
        std::string name;
        std::uint64_t value{0};
        stream >> name >> value;
        if (name == "Rss:") {
            snapshot.rssKiB = value;
        } else if (name == "Pss:") {
            snapshot.pssKiB = value;
        } else if (name == "Private_Clean:" || name == "Private_Dirty:") {
            snapshot.privateKiB += value;
        }
    }
    return snapshot;
}

void PrintSnapshot(const char* stage, const ProcessMemorySnapshot& snapshot)
{
    std::cout << "asu_client_process_memory: stage=" << stage
              << " heap_in_use_bytes=" << snapshot.heapInUseBytes
              << " heap_mapped_bytes=" << snapshot.heapMappedBytes << " rss_kib=" << snapshot.rssKiB
              << " pss_kib=" << snapshot.pssKiB << " private_kib=" << snapshot.privateKiB << '\n';
}

void PrintDelta(const char* stage, const ProcessMemorySnapshot& before,
                const ProcessMemorySnapshot& after)
{
    const auto delta = [](std::uint64_t first, std::uint64_t second) {
        return static_cast<std::int64_t>(second) - static_cast<std::int64_t>(first);
    };
    std::cout << "asu_client_process_memory: stage=" << stage
              << " heap_in_use_bytes=" << delta(before.heapInUseBytes, after.heapInUseBytes)
              << " heap_mapped_bytes=" << delta(before.heapMappedBytes, after.heapMappedBytes)
              << " rss_kib=" << delta(before.rssKiB, after.rssKiB)
              << " pss_kib=" << delta(before.pssKiB, after.pssKiB)
              << " private_kib=" << delta(before.privateKiB, after.privateKiB) << '\n';
}

void PrintOwnedUsage(const AsuClientMemoryUsage& usage)
{
    std::cout << "asu_client_owned_memory:"
              << " object_bytes=" << usage.objectBytes << " config_bytes=" << usage.configBytes
              << " snapshot_bytes=" << usage.snapshotBytes
              << " registered_resource_bytes=" << usage.registeredResourceBytes
              << " task_bytes=" << usage.taskBytes
              << " total_estimated_bytes=" << usage.totalEstimatedBytes
              << " transport_count=" << usage.transportCount
              << " router_count=" << usage.routerCount
              << " view_server_count=" << usage.viewServerCount
              << " live_task_count=" << usage.liveTaskCount << '\n';
}

// Run this probe alone in a fresh process:
// ASU_CLIENT_MEMORY_PROBE_CONFIG=ucm/transport/kv/asu/test/case/asu_client_memory_probe.conf \
//   ./asu.test --gtest_filter=AsuClientMemoryProbe.IdleClientInitialization
TEST(AsuClientMemoryProbe, IdleClientInitialization)
{
    const auto* configPath = std::getenv(kProbeConfigEnv);
    if (configPath == nullptr || configPath[0] == '\0') {
        GTEST_SKIP() << kProbeConfigEnv << " is not set";
    }

    const auto beforeCreate = TakeProcessMemorySnapshot();
    PrintSnapshot("before_create", beforeCreate);

    auto client = CreateAsuClient();
    ASSERT_NE(client, nullptr);
    const auto afterCreate = TakeProcessMemorySnapshot();
    PrintSnapshot("after_create", afterCreate);

    const auto status = client->Init(configPath);
    ASSERT_TRUE(status.ok()) << status.message;
    std::this_thread::sleep_for(kStabilizationDelay);
    const auto afterInit = TakeProcessMemorySnapshot();
    PrintSnapshot("after_init_stable", afterInit);
    PrintOwnedUsage(client->GetMemoryUsage());

    const auto shutdownStatus = client->Shutdown();
    EXPECT_TRUE(shutdownStatus.ok()) << shutdownStatus.message;
    client.reset();
    std::this_thread::sleep_for(kStabilizationDelay);
    const auto afterShutdown = TakeProcessMemorySnapshot();
    PrintSnapshot("after_shutdown_stable", afterShutdown);

    PrintDelta("create_delta", beforeCreate, afterCreate);
    PrintDelta("init_delta", afterCreate, afterInit);
    PrintDelta("retained_after_shutdown_delta", beforeCreate, afterShutdown);
}

}  // namespace
}  // namespace UC::ASU
