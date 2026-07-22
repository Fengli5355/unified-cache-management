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
#include <acl/acl.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "aiv_transport/aiv_transport.h"

namespace UC::ASU {
namespace {

constexpr std::uint32_t kDeviceId = 0;
constexpr std::size_t kMemorySize = 2 * 1024 * 1024;
constexpr std::size_t kBaselineProviderCount = 2;
constexpr std::size_t kRetryProviderCount = 4;
constexpr std::uint32_t kQpNum = 1;
constexpr std::uint32_t kConnectionTimeoutMs = 5000;

struct Registration {
    MRHandle handle{kInvalidMRHandle};
    std::uint32_t tokenId{0};
    bool registered{false};
    bool tokenAvailable{false};
};

void Trace(const std::string& message)
{
    std::cerr << "[AIV repeat registration] " << message << std::endl;
}

class AivRepeatRegistrationTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        Trace("calling aclInit");
        const auto initStatus = aclInit(nullptr);
        Trace("aclInit returned " + std::to_string(initStatus));
        ASSERT_EQ(initStatus, ACL_SUCCESS);
        Trace("calling aclrtSetDevice");
        const auto setDeviceStatus = aclrtSetDevice(kDeviceId);
        Trace("aclrtSetDevice returned " + std::to_string(setDeviceStatus));
        ASSERT_EQ(setDeviceStatus, ACL_SUCCESS);
    }

    static void TearDownTestSuite()
    {
        Trace("calling aclrtResetDevice");
        const auto resetDeviceStatus = aclrtResetDevice(kDeviceId);
        Trace("aclrtResetDevice returned " + std::to_string(resetDeviceStatus));
        EXPECT_EQ(resetDeviceStatus, ACL_SUCCESS);
    }

    void SetUp() override
    {
        const char* localIp = std::getenv("AIV_TEST_LOCAL_IP");
        const char* remoteIp = std::getenv("AIV_TEST_REMOTE_IP");
        const char* portText = std::getenv("AIV_TEST_PORT");
        ASSERT_NE(localIp, nullptr) << "AIV_TEST_LOCAL_IP is not set";
        ASSERT_NE(remoteIp, nullptr) << "AIV_TEST_REMOTE_IP is not set";
        ASSERT_NE(portText, nullptr) << "AIV_TEST_PORT is not set";
        localIp_ = localIp;
        remoteIp_ = remoteIp;
        port_ = static_cast<std::uint32_t>(std::stoul(portText));
        Trace("endpoint local=" + localIp_ + " remote=" + remoteIp_ + ":" + portText);

        Trace("calling aclrtMalloc");
        const auto mallocStatus =
            aclrtMalloc(&deviceMemory_, kMemorySize, ACL_MEM_TYPE_HIGH_BAND_WIDTH);
        Trace("aclrtMalloc returned " + std::to_string(mallocStatus) +
              " addr=" + std::to_string(reinterpret_cast<std::uintptr_t>(deviceMemory_)));
        ASSERT_EQ(mallocStatus, ACL_SUCCESS);
    }

    void TearDown() override
    {
        if (deviceMemory_ == nullptr) { return; }
        Trace("calling aclrtFree");
        const auto freeStatus = aclrtFree(deviceMemory_);
        Trace("aclrtFree returned " + std::to_string(freeStatus));
        EXPECT_EQ(freeStatus, ACL_SUCCESS);
    }

    std::string localIp_;
    std::string remoteIp_;
    std::uint32_t port_{0};
    void* deviceMemory_{nullptr};
};

TEST_F(AivRepeatRegistrationTest, MultipleProvidersRegisterAndUnregisterSameDeviceMemory)
{
    Trace("baseline test body entered");

    std::vector<std::unique_ptr<AIVTransport>> providers;
    std::vector<std::vector<AIVTransport::ConnectionHandle>> connections(kBaselineProviderCount);
    std::vector<Registration> registrations(kBaselineProviderCount);
    providers.reserve(kBaselineProviderCount);

    for (std::size_t i = 0; i < kBaselineProviderCount; ++i) {
        Trace("provider[" + std::to_string(i) + "] calling CreateAIVTransProvider");
        providers.push_back(CreateAIVTransProvider(kDeviceId));
        Trace("provider[" + std::to_string(i) + "] CreateAIVTransProvider returned " +
              (providers.back() == nullptr ? "null" : "non-null"));
        EXPECT_NE(providers.back(), nullptr) << "failed to create provider " << i;
        if (providers.back() == nullptr) { continue; }

        Trace("provider[" + std::to_string(i) + "] calling CreateConnection");
        const auto connectionStatus = providers.back()->CreateConnection(
            localIp_, remoteIp_, port_, kQpNum, kConnectionTimeoutMs, connections[i]);
        Trace("provider[" + std::to_string(i) + "] CreateConnection returned code=" +
              std::to_string(static_cast<int>(connectionStatus.code)) + " handles=" +
              std::to_string(connections[i].size()) + " message=" + connectionStatus.message);
        EXPECT_TRUE(connectionStatus.ok())
            << "provider " << i << " create connection failed: " << connectionStatus.message;
        EXPECT_EQ(connections[i].size(), kQpNum)
            << "provider " << i << " returned unexpected connection count";
        if (!connectionStatus.ok() || connections[i].size() != kQpNum) { continue; }

        const std::vector<AIVTransport::RegisterMemoryDesc> descs{
            {AIVTransport::MemType::MEM_DEVICE, reinterpret_cast<std::uintptr_t>(deviceMemory_),
             kMemorySize},
        };
        std::vector<MRHandle> handles;
        Trace("provider[" + std::to_string(i) + "] calling RegisterMemory");
        const auto registerStatus = providers.back()->RegisterMemory(descs, handles);
        Trace("provider[" + std::to_string(i) + "] RegisterMemory returned code=" +
              std::to_string(static_cast<int>(registerStatus.code)) +
              " handles=" + std::to_string(handles.size()) + " message=" + registerStatus.message);
        EXPECT_TRUE(registerStatus.ok())
            << "provider " << i << " register failed: " << registerStatus.message;
        if (!registerStatus.ok()) { continue; }

        EXPECT_EQ(handles.size(), 1U) << "provider " << i << " returned unexpected handle count";
        if (handles.size() != 1U) { continue; }
        registrations[i].handle = handles.front();
        registrations[i].registered = true;

        Trace("provider[" + std::to_string(i) + "] calling GetMemTokenId");
        const auto tokenStatus =
            providers.back()->GetMemTokenId(registrations[i].handle, registrations[i].tokenId);
        Trace("provider[" + std::to_string(i) + "] GetMemTokenId returned code=" +
              std::to_string(static_cast<int>(tokenStatus.code)) + " token_id=" +
              std::to_string(registrations[i].tokenId) + " message=" + tokenStatus.message);
        registrations[i].tokenAvailable = tokenStatus.ok();
        EXPECT_TRUE(tokenStatus.ok())
            << "provider " << i << " get token failed: " << tokenStatus.message;
        std::cout << "provider[" << i << "]: handle=" << registrations[i].handle;
        if (registrations[i].tokenAvailable) {
            std::cout << ", token_id=" << registrations[i].tokenId;
        } else {
            std::cout << ", token_id=unavailable";
        }
        std::cout << '\n';
    }

    for (std::size_t i = 1; i < registrations.size(); ++i) {
        if (!registrations[0].registered || !registrations[i].registered) { continue; }
        std::cout << "provider[0] vs provider[" << i << "]: handle="
                  << (registrations[0].handle == registrations[i].handle ? "same" : "different")
                  << ", token_id=";
        if (registrations[0].tokenAvailable && registrations[i].tokenAvailable) {
            std::cout << (registrations[0].tokenId == registrations[i].tokenId ? "same"
                                                                               : "different");
        } else {
            std::cout << "unavailable";
        }
        std::cout << '\n';
    }

    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (!registrations[i].registered) { continue; }
        Trace("provider[" + std::to_string(i) + "] calling UnregisterMemory");
        const auto statuses = providers[i]->UnregisterMemory({{registrations[i].handle}});
        Trace("provider[" + std::to_string(i) +
              "] UnregisterMemory returned statuses=" + std::to_string(statuses.size()));
        EXPECT_EQ(statuses.size(), 1U) << "provider " << i << " returned unexpected status count";
        if (statuses.size() != 1U) { continue; }
        Trace("provider[" + std::to_string(i) + "] UnregisterMemory status code=" +
              std::to_string(static_cast<int>(statuses.front().code)) +
              " message=" + statuses.front().message);
        EXPECT_TRUE(statuses.front().ok())
            << "provider " << i << " unregister failed: " << statuses.front().message;
    }

    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (providers[i] == nullptr || connections[i].empty()) { continue; }
        Trace("provider[" + std::to_string(i) + "] calling DeleteConnections");
        const auto statuses = providers[i]->DeleteConnections(connections[i]);
        Trace("provider[" + std::to_string(i) +
              "] DeleteConnections returned statuses=" + std::to_string(statuses.size()));
        EXPECT_EQ(statuses.size(), connections[i].size())
            << "provider " << i << " returned unexpected delete connection status count";
        for (std::size_t statusIndex = 0; statusIndex < statuses.size(); ++statusIndex) {
            const auto& status = statuses[statusIndex];
            Trace("provider[" + std::to_string(i) + "] DeleteConnections status[" +
                  std::to_string(statusIndex) + "] code=" +
                  std::to_string(static_cast<int>(status.code)) + " message=" + status.message);
            EXPECT_TRUE(status.ok())
                << "provider " << i << " delete connection failed: " << status.message;
        }
    }

    providers.clear();
    Trace("baseline test body completed");
}

TEST_F(AivRepeatRegistrationTest, ThirdProviderRegistersAfterOneRegistrationIsReleased)
{
    Trace("retry-after-unregister test body entered");
    std::vector<std::unique_ptr<AIVTransport>> providers;
    std::vector<std::vector<AIVTransport::ConnectionHandle>> connections(kRetryProviderCount);
    std::vector<Registration> registrations(kRetryProviderCount);
    providers.reserve(kRetryProviderCount);

    for (std::size_t i = 0; i < kRetryProviderCount; ++i) {
        Trace("retry provider[" + std::to_string(i) + "] calling CreateAIVTransProvider");
        providers.push_back(CreateAIVTransProvider(kDeviceId));
        ASSERT_NE(providers.back(), nullptr) << "failed to create provider " << i;

        Trace("retry provider[" + std::to_string(i) + "] calling CreateConnection");
        const auto connectionStatus = providers.back()->CreateConnection(
            localIp_, remoteIp_, port_, kQpNum, kConnectionTimeoutMs, connections[i]);
        Trace("retry provider[" + std::to_string(i) + "] CreateConnection returned code=" +
              std::to_string(static_cast<int>(connectionStatus.code)) + " handles=" +
              std::to_string(connections[i].size()) + " message=" + connectionStatus.message);
        ASSERT_TRUE(connectionStatus.ok())
            << "provider " << i << " create connection failed: " << connectionStatus.message;
        ASSERT_EQ(connections[i].size(), kQpNum);
    }

    const std::vector<AIVTransport::RegisterMemoryDesc> descs{
        {AIVTransport::MemType::MEM_DEVICE, reinterpret_cast<std::uintptr_t>(deviceMemory_),
         kMemorySize},
    };

    for (std::size_t i = 0; i < kBaselineProviderCount; ++i) {
        std::vector<MRHandle> handles;
        Trace("retry provider[" + std::to_string(i) + "] calling RegisterMemory");
        const auto registerStatus = providers[i]->RegisterMemory(descs, handles);
        Trace("retry provider[" + std::to_string(i) + "] RegisterMemory returned code=" +
              std::to_string(static_cast<int>(registerStatus.code)) +
              " handles=" + std::to_string(handles.size()) + " message=" + registerStatus.message);
        ASSERT_TRUE(registerStatus.ok()) << registerStatus.message;
        ASSERT_EQ(handles.size(), 1U);
        registrations[i].handle = handles.front();
        registrations[i].registered = true;

        Trace("retry provider[" + std::to_string(i) + "] calling GetMemTokenId");
        const auto tokenStatus =
            providers[i]->GetMemTokenId(registrations[i].handle, registrations[i].tokenId);
        Trace("retry provider[" + std::to_string(i) + "] GetMemTokenId returned code=" +
              std::to_string(static_cast<int>(tokenStatus.code)) + " token_id=" +
              std::to_string(registrations[i].tokenId) + " message=" + tokenStatus.message);
        ASSERT_TRUE(tokenStatus.ok()) << tokenStatus.message;
        registrations[i].tokenAvailable = true;
    }

    std::vector<MRHandle> thirdHandles;
    Trace("retry provider[2] calling concurrent RegisterMemory; failure is expected");
    const auto concurrentStatus = providers[2]->RegisterMemory(descs, thirdHandles);
    Trace("retry provider[2] concurrent RegisterMemory returned code=" +
          std::to_string(static_cast<int>(concurrentStatus.code)) + " handles=" +
          std::to_string(thirdHandles.size()) + " message=" + concurrentStatus.message);
    EXPECT_FALSE(concurrentStatus.ok());
    EXPECT_TRUE(thirdHandles.empty());
    if (!thirdHandles.empty()) {
        const auto statuses = providers[2]->UnregisterMemory({{thirdHandles.front()}});
        ASSERT_EQ(statuses.size(), 1U);
        ASSERT_TRUE(statuses.front().ok()) << statuses.front().message;
    }

    Trace("retry provider[0] calling UnregisterMemory to release one registration");
    const auto firstUnregisterStatuses =
        providers[0]->UnregisterMemory({{registrations[0].handle}});
    ASSERT_EQ(firstUnregisterStatuses.size(), 1U);
    Trace("retry provider[0] UnregisterMemory status code=" +
          std::to_string(static_cast<int>(firstUnregisterStatuses.front().code)) +
          " message=" + firstUnregisterStatuses.front().message);
    ASSERT_TRUE(firstUnregisterStatuses.front().ok()) << firstUnregisterStatuses.front().message;
    registrations[0].registered = false;

    thirdHandles.clear();
    Trace("retry provider[2] calling RegisterMemory after provider[0] unregister");
    const auto retryStatus = providers[2]->RegisterMemory(descs, thirdHandles);
    Trace("retry provider[2] RegisterMemory after release returned code=" +
          std::to_string(static_cast<int>(retryStatus.code)) +
          " handles=" + std::to_string(thirdHandles.size()) + " message=" + retryStatus.message);
    EXPECT_TRUE(retryStatus.ok()) << retryStatus.message;
    EXPECT_EQ(thirdHandles.size(), 1U);
    if (retryStatus.ok() && thirdHandles.size() == 1U) {
        registrations[2].handle = thirdHandles.front();
        registrations[2].registered = true;
        const auto tokenStatus =
            providers[2]->GetMemTokenId(registrations[2].handle, registrations[2].tokenId);
        Trace("retry provider[2] GetMemTokenId returned code=" +
              std::to_string(static_cast<int>(tokenStatus.code)) + " token_id=" +
              std::to_string(registrations[2].tokenId) + " message=" + tokenStatus.message);
        EXPECT_TRUE(tokenStatus.ok()) << tokenStatus.message;
        if (tokenStatus.ok()) {
            registrations[2].tokenAvailable = true;
            EXPECT_NE(registrations[2].handle, kInvalidMRHandle);
            Trace("retry provider[1] handle=" + std::to_string(registrations[1].handle) +
                  " provider[2] handle=" + std::to_string(registrations[2].handle) +
                  "; handles are provider-local and may differ");
            EXPECT_EQ(registrations[2].tokenId, registrations[1].tokenId);
        }
    }

    Trace("retry provider[1] calling UnregisterMemory to release one registration");
    const auto secondUnregisterStatuses =
        providers[1]->UnregisterMemory({{registrations[1].handle}});
    ASSERT_EQ(secondUnregisterStatuses.size(), 1U);
    Trace("retry provider[1] UnregisterMemory status code=" +
          std::to_string(static_cast<int>(secondUnregisterStatuses.front().code)) +
          " message=" + secondUnregisterStatuses.front().message);
    ASSERT_TRUE(secondUnregisterStatuses.front().ok()) << secondUnregisterStatuses.front().message;
    registrations[1].registered = false;

    std::vector<MRHandle> fourthHandles;
    Trace("retry provider[3] calling first RegisterMemory after provider[1] unregister");
    const auto fourthStatus = providers[3]->RegisterMemory(descs, fourthHandles);
    Trace("retry provider[3] first RegisterMemory returned code=" +
          std::to_string(static_cast<int>(fourthStatus.code)) +
          " handles=" + std::to_string(fourthHandles.size()) + " message=" + fourthStatus.message);
    EXPECT_TRUE(fourthStatus.ok()) << fourthStatus.message;
    EXPECT_EQ(fourthHandles.size(), 1U);
    if (fourthStatus.ok() && fourthHandles.size() == 1U) {
        registrations[3].handle = fourthHandles.front();
        registrations[3].registered = true;
        EXPECT_NE(registrations[3].handle, kInvalidMRHandle);

        Trace("retry provider[3] calling GetMemTokenId");
        const auto tokenStatus =
            providers[3]->GetMemTokenId(registrations[3].handle, registrations[3].tokenId);
        Trace("retry provider[3] GetMemTokenId returned code=" +
              std::to_string(static_cast<int>(tokenStatus.code)) + " token_id=" +
              std::to_string(registrations[3].tokenId) + " message=" + tokenStatus.message);
        EXPECT_TRUE(tokenStatus.ok()) << tokenStatus.message;
        if (tokenStatus.ok()) {
            registrations[3].tokenAvailable = true;
            if (registrations[2].tokenAvailable) {
                EXPECT_EQ(registrations[3].tokenId, registrations[2].tokenId);
            }
        }
    }

    for (std::size_t i = 1; i < registrations.size(); ++i) {
        if (!registrations[i].registered) { continue; }
        Trace("retry provider[" + std::to_string(i) + "] calling cleanup UnregisterMemory");
        const auto statuses = providers[i]->UnregisterMemory({{registrations[i].handle}});
        ASSERT_EQ(statuses.size(), 1U);
        EXPECT_TRUE(statuses.front().ok()) << statuses.front().message;
    }

    for (std::size_t i = 0; i < providers.size(); ++i) {
        Trace("retry provider[" + std::to_string(i) + "] calling DeleteConnections");
        const auto statuses = providers[i]->DeleteConnections(connections[i]);
        EXPECT_EQ(statuses.size(), connections[i].size());
        for (const auto& status : statuses) { EXPECT_TRUE(status.ok()) << status.message; }
    }
    providers.clear();
    Trace("retry-after-unregister test body completed");
}

}  // namespace
}  // namespace UC::ASU
