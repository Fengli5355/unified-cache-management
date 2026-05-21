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
#pragma once
#include <vector>
#include "asu_transport/types.h"

namespace UC::ASU {

struct AsuClientConfig;

// AsuClient provides the public asynchronous KV client interface.
class AsuClient {
public:
    // Destroys the client interface.
    virtual ~AsuClient() = default;

    // Initializes the client from a concrete implementation config.
    virtual Status Init(const AsuClientConfig& config) = 0;
    // Stops accepting new operations and releases client-side resources.
    virtual Status Shutdown() = 0;

    // Queries key existence through the current routing view.
    virtual Status Query(const std::vector<CacheKey>& keys, const QueryOptions& options,
                         QueryResult& result) = 0;

    // Submits asynchronous load operations.
    virtual Status LoadAsync(const std::vector<KVBuffer>& entries, TaskId& taskId) = 0;
    // Submits asynchronous store operations.
    virtual Status StoreAsync(const std::vector<KVBuffer>& entries, TaskId& taskId) = 0;
    // Submits asynchronous delete operations.
    virtual Status DeleteAsync(const std::vector<CacheKey>& keys, TaskId& taskId) = 0;

    // Checks an asynchronous aggregate task without blocking.
    virtual Status Check(TaskId taskId, TaskResult& result) = 0;
    // Waits for an asynchronous aggregate task up to the provided timeout.
    virtual Status Wait(TaskId taskId, std::uint64_t timeoutMs, TaskResult& result) = 0;

    // Registers memory regions across the current view.
    virtual Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                                   std::vector<RegisterResult>& results) = 0;
    // Unregisters memory regions across the current view.
    virtual Status UnregisterRegions(const std::vector<MRHandle>& handles) = 0;
};

}  // namespace UC::ASU
