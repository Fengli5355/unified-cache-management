/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
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
#ifndef UNIFIEDCACHE_ASUSTORE_VIEW_MGR_H
#define UNIFIEDCACHE_ASUSTORE_VIEW_MGR_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace UC::AsuStore {

struct AsuView {
    std::string kvnsId;
    uint64_t version{0};
    std::vector<uint64_t> asuIds;
};

using AsuViewPtr = std::shared_ptr<const AsuView>;
using ViewChangeCallback = std::function<void(const AsuView& oldView, const AsuView& newView,
                                              const std::vector<uint64_t>& addedAsuIds,
                                              const std::vector<uint64_t>& removedAsuIds)>;

struct ViewMgrConfig {
    std::string kvnsId;
    uint64_t refreshIntervalMs{30000};
    bool enablePeriodicRefresh{true};
    ViewChangeCallback viewChangeCallback;
};

class ViewServer {
public:
    explicit ViewServer(std::string configPath);
    virtual ~ViewServer() = default;

    virtual AsuView FetchAsuView(const std::string& kvnsId) const;

private:
    std::string configPath_;
};

class ViewMgr {
public:
    explicit ViewMgr(std::shared_ptr<ViewServer> viewServer);
    ViewMgr(std::shared_ptr<ViewServer> viewServer, ViewMgrConfig config);
    explicit ViewMgr(const std::string& configPath);
    ViewMgr(const std::string& configPath, ViewMgrConfig config);
    ~ViewMgr();

    ViewMgr(const ViewMgr&) = delete;
    ViewMgr& operator=(const ViewMgr&) = delete;

    AsuViewPtr GetView() const;
    std::vector<uint64_t> GetAsuIds() const;
    std::string GetKvnsId() const;
    uint64_t GetVersion() const;
    uint64_t GetConsecutiveRefreshFailures() const;

    void Refresh();
    void TriggerRefresh();
    void NotifyConnectionFailure();
    void NotifyViewExpired();
    void Stop();

private:
    void StartPeriodicRefresh();
    void RefreshLoop();
    void PublishView(AsuView view);
    static std::vector<uint64_t> GetAddedAsuIds(const std::vector<uint64_t>& oldAsuIds,
                                                const std::vector<uint64_t>& newAsuIds);
    static std::vector<uint64_t> GetRemovedAsuIds(const std::vector<uint64_t>& oldAsuIds,
                                                  const std::vector<uint64_t>& newAsuIds);

    std::shared_ptr<ViewServer> viewServer_;
    ViewMgrConfig config_;
    mutable std::mutex viewMutex_;
    mutable std::mutex refreshMutex_;
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    std::thread refreshThread_;
    bool stopped_{false};
    std::atomic<uint64_t> consecutiveRefreshFailures_{0};
    AsuViewPtr view_;
};

}  // namespace UC::AsuStore

#endif
