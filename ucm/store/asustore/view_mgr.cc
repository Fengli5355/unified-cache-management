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
#include "view_mgr.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace UC::AsuStore {
namespace {

std::string Trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) { return ""; }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> Split(const std::string& value, char delimiter)
{
    std::vector<std::string> parts;
    std::stringstream stream{value};
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        part = Trim(part);
        if (!part.empty()) { parts.emplace_back(std::move(part)); }
    }
    return parts;
}

bool HasAsuId(const std::vector<uint64_t>& asuIds, uint64_t asuId)
{
    return std::find(asuIds.begin(), asuIds.end(), asuId) != asuIds.end();
}

}  // namespace

ViewServer::ViewServer(std::string configPath) : configPath_(std::move(configPath)) {}

AsuView ViewServer::FetchAsuView(const std::string& kvnsId) const
{
    std::ifstream configFile{configPath_};
    if (!configFile.is_open()) {
        throw std::runtime_error("failed to open asu view config file: " + configPath_);
    }

    AsuView view;
    view.kvnsId = kvnsId;
    bool hasConfigKvnsId = false;
    std::string line;
    while (std::getline(configFile, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') { continue; }

        const auto pos = line.find('=');
        if (pos == std::string::npos) { continue; }

        const auto key = Trim(line.substr(0, pos));
        const auto value = Trim(line.substr(pos + 1));
        if (key == "kvns_id") {
            hasConfigKvnsId = true;
            if (!kvnsId.empty() && value != kvnsId) {
                throw std::runtime_error("asu view config kvns_id mismatch: " + value);
            }
            view.kvnsId = value;
        } else if (key == "view_version") {
            view.version = std::stoull(value);
        } else if (key == "asu_ids") {
            view.asuIds.clear();
            for (const auto& asuId : Split(value, ',')) {
                view.asuIds.emplace_back(std::stoull(asuId));
            }
        }
    }

    if (!hasConfigKvnsId && !kvnsId.empty()) { view.kvnsId = kvnsId; }
    return view;
}

ViewMgr::ViewMgr(std::shared_ptr<ViewServer> viewServer)
    : ViewMgr(std::move(viewServer), ViewMgrConfig{})
{
}

ViewMgr::ViewMgr(std::shared_ptr<ViewServer> viewServer, ViewMgrConfig config)
    : viewServer_(std::move(viewServer)),
      config_(std::move(config)),
      view_(std::make_shared<AsuView>())
{
    if (viewServer_ == nullptr) { throw std::invalid_argument("view server is null"); }
    Refresh();
    StartPeriodicRefresh();
}

ViewMgr::ViewMgr(const std::string& configPath)
    : ViewMgr(std::make_shared<ViewServer>(configPath), ViewMgrConfig{})
{
}

ViewMgr::ViewMgr(const std::string& configPath, ViewMgrConfig config)
    : ViewMgr(std::make_shared<ViewServer>(configPath), std::move(config))
{
}

ViewMgr::~ViewMgr()
{
    Stop();
}

AsuViewPtr ViewMgr::GetView() const
{
    std::lock_guard<std::mutex> lock{viewMutex_};
    return view_;
}

std::vector<uint64_t> ViewMgr::GetAsuIds() const
{
    auto view = GetView();
    return view->asuIds;
}

std::string ViewMgr::GetKvnsId() const
{
    auto view = GetView();
    return view->kvnsId;
}

uint64_t ViewMgr::GetVersion() const
{
    auto view = GetView();
    return view->version;
}

uint64_t ViewMgr::GetConsecutiveRefreshFailures() const
{
    return consecutiveRefreshFailures_.load();
}

void ViewMgr::Refresh()
{
    std::lock_guard<std::mutex> lock{refreshMutex_};
    PublishView(viewServer_->FetchAsuView(config_.kvnsId));
    consecutiveRefreshFailures_.store(0);
}

void ViewMgr::TriggerRefresh()
{
    try {
        Refresh();
    } catch (...) {
        consecutiveRefreshFailures_.fetch_add(1);
    }
}

void ViewMgr::NotifyConnectionFailure()
{
    TriggerRefresh();
}

void ViewMgr::NotifyViewExpired()
{
    TriggerRefresh();
}

void ViewMgr::Stop()
{
    {
        std::lock_guard<std::mutex> lock{stopMutex_};
        if (stopped_) { return; }
        stopped_ = true;
    }

    stopCv_.notify_all();
    if (refreshThread_.joinable()) { refreshThread_.join(); }
}

void ViewMgr::StartPeriodicRefresh()
{
    if (!config_.enablePeriodicRefresh || config_.refreshIntervalMs == 0) { return; }
    refreshThread_ = std::thread(&ViewMgr::RefreshLoop, this);
}

void ViewMgr::RefreshLoop()
{
    std::unique_lock<std::mutex> lock{stopMutex_};
    while (!stopped_) {
        const auto interval = std::chrono::milliseconds(config_.refreshIntervalMs);
        if (stopCv_.wait_for(lock, interval, [this] { return stopped_; })) { break; }

        lock.unlock();
        try {
            Refresh();
        } catch (...) {
            consecutiveRefreshFailures_.fetch_add(1);
        }
        lock.lock();
    }
}

void ViewMgr::PublishView(AsuView view)
{
    auto newView = std::make_shared<AsuView>(std::move(view));
    AsuViewPtr oldView;
    {
        std::lock_guard<std::mutex> lock{viewMutex_};
        oldView = view_;
        view_ = newView;
    }

    if (oldView == nullptr || oldView->version == newView->version) { return; }
    auto addedAsuIds = GetAddedAsuIds(oldView->asuIds, newView->asuIds);
    auto removedAsuIds = GetRemovedAsuIds(oldView->asuIds, newView->asuIds);
    if (addedAsuIds.empty() && removedAsuIds.empty()) { return; }
    if (config_.viewChangeCallback) {
        config_.viewChangeCallback(*oldView, *newView, addedAsuIds, removedAsuIds);
    }
}

std::vector<uint64_t> ViewMgr::GetAddedAsuIds(const std::vector<uint64_t>& oldAsuIds,
                                              const std::vector<uint64_t>& newAsuIds)
{
    std::vector<uint64_t> addedAsuIds;
    for (auto asuId : newAsuIds) {
        if (!HasAsuId(oldAsuIds, asuId) && !HasAsuId(addedAsuIds, asuId)) {
            addedAsuIds.emplace_back(asuId);
        }
    }
    return addedAsuIds;
}

std::vector<uint64_t> ViewMgr::GetRemovedAsuIds(const std::vector<uint64_t>& oldAsuIds,
                                                const std::vector<uint64_t>& newAsuIds)
{
    std::vector<uint64_t> removedAsuIds;
    for (auto asuId : oldAsuIds) {
        if (!HasAsuId(newAsuIds, asuId) && !HasAsuId(removedAsuIds, asuId)) {
            removedAsuIds.emplace_back(asuId);
        }
    }
    return removedAsuIds;
}

}  // namespace UC::AsuStore
