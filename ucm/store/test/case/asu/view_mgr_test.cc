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
#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "view_mgr.h"

namespace {

using UC::AsuStore::AsuView;
using UC::AsuStore::ViewMgr;
using UC::AsuStore::ViewMgrConfig;
using UC::AsuStore::ViewServer;

std::string GetCurrentDirFromFile(const std::string& file)
{
    auto pos = file.find_last_of("/\\");
    if (pos == std::string::npos) { return "."; }
    return file.substr(0, pos);
}

class FakeViewServer : public ViewServer {
public:
    explicit FakeViewServer(std::vector<AsuView> views)
        : ViewServer("unused"),
          views_(std::move(views))
    {
    }

    AsuView FetchAsuView(const std::string& kvnsId) const override
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (views_.empty()) { throw std::runtime_error("empty fake view"); }

        auto index = fetchCount_;
        if (index >= views_.size()) { index = views_.size() - 1; }
        ++fetchCount_;

        auto view = views_[index];
        view.kvnsId = kvnsId;
        return view;
    }

private:
    mutable std::mutex mutex_;
    mutable size_t fetchCount_{0};
    std::vector<AsuView> views_;
};

ViewMgrConfig MakeManualConfig(const std::string& kvnsId)
{
    ViewMgrConfig config;
    config.kvnsId = kvnsId;
    config.enablePeriodicRefresh = false;
    return config;
}

}  // namespace

TEST(ViewMgrTest, LoadAsuViewFromConfig)
{
    const auto configPath = GetCurrentDirFromFile(__FILE__) + "/view_mgr_test.config";
    auto config = MakeManualConfig("kvns-test");

    ViewMgr viewMgr{configPath, config};

    EXPECT_EQ(viewMgr.GetKvnsId(), "kvns-test");
    EXPECT_EQ(viewMgr.GetVersion(), uint64_t{7});
    EXPECT_EQ(viewMgr.GetAsuIds(), std::vector<uint64_t>({10, 20, 30}));

    const auto view = viewMgr.GetView();
    EXPECT_EQ(view->kvnsId, "kvns-test");
    EXPECT_EQ(view->version, uint64_t{7});
    EXPECT_EQ(view->asuIds, std::vector<uint64_t>({10, 20, 30}));
}

TEST(ViewMgrTest, RejectNullViewServer)
{
    auto body = [] {
        ViewMgr viewMgr{std::shared_ptr<ViewServer>{}, MakeManualConfig("kvns-test")};
    };

    EXPECT_THROW(body(), std::invalid_argument);
}

TEST(ViewMgrTest, ThrowWhenConfigFileMissing)
{
    auto body = [] {
        ViewMgr viewMgr{"missing_asu_view.config", MakeManualConfig("kvns-test")};
    };

    EXPECT_THROW(body(), std::runtime_error);
}

TEST(ViewMgrTest, RejectMismatchedKvnsId)
{
    const auto configPath = GetCurrentDirFromFile(__FILE__) + "/view_mgr_test.config";
    auto body = [&configPath] {
        ViewMgr viewMgr{configPath, MakeManualConfig("another-kvns")};
    };

    EXPECT_THROW(body(), std::runtime_error);
}

TEST(ViewMgrTest, TriggerRefreshKeepsOldViewAlive)
{
    auto viewServer = std::make_shared<FakeViewServer>(std::vector<AsuView>{
        AsuView{"", 1, {10, 20}},
        AsuView{"", 2, {20, 30}},
    });
    ViewMgr viewMgr{viewServer, MakeManualConfig("kvns-test")};

    auto oldView = viewMgr.GetView();
    viewMgr.TriggerRefresh();
    auto newView = viewMgr.GetView();

    EXPECT_EQ(oldView->version, uint64_t{1});
    EXPECT_EQ(oldView->asuIds, std::vector<uint64_t>({10, 20}));
    EXPECT_EQ(newView->version, uint64_t{2});
    EXPECT_EQ(newView->asuIds, std::vector<uint64_t>({20, 30}));
}

TEST(ViewMgrTest, NotifyConnectorManagerWhenViewChanges)
{
    auto config = MakeManualConfig("kvns-test");
    std::vector<uint64_t> addedAsuIds;
    std::vector<uint64_t> removedAsuIds;
    config.viewChangeCallback = [&addedAsuIds, &removedAsuIds](
                                    const AsuView&, const AsuView&,
                                    const std::vector<uint64_t>& added,
                                    const std::vector<uint64_t>& removed) {
        addedAsuIds = added;
        removedAsuIds = removed;
    };

    auto viewServer = std::make_shared<FakeViewServer>(std::vector<AsuView>{
        AsuView{"", 1, {10, 20}},
        AsuView{"", 2, {20, 30}},
    });
    ViewMgr viewMgr{viewServer, config};

    addedAsuIds.clear();
    removedAsuIds.clear();
    viewMgr.NotifyViewExpired();

    EXPECT_EQ(addedAsuIds, std::vector<uint64_t>({30}));
    EXPECT_EQ(removedAsuIds, std::vector<uint64_t>({10}));
}
