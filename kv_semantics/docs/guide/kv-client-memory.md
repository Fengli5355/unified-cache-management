# KVClient 内存与 Buffer 约定

KVClient 的数据操作不复制业务 buffer，也不接管其所有权。调用方负责分配内存、注册 region、提交包含 handle 的 `KVBuffer`，并在任务完成后再释放或注销。这一生命周期是正确使用 KVClient 的核心。

## 1. 基本模型

```text
业务内存
  -> MemoryRegion：描述地址范围
  -> RegisterRegions：注册到 provider
  -> RegisteredMemory：返回 canonical handle 和 token
  -> KVBuffer：引用 region、handle、key 和远端 offset
  -> StoreAsync / LoadAsync
  -> Wait 完成
  -> UnregisterRegions
  -> 释放业务内存
```

`RegisterRegions()` 不分配业务内存。它只让底层 provider 能够访问已经存在的地址范围。

## 2. MemoryRegion 字段

```cpp
struct MemoryRegion {
    MemoryType memoryType;
    std::uint64_t addr;
    std::uint64_t size;
    std::int32_t deviceId;
    std::int32_t numaNode;
};
```

| 字段 | 含义 | 约定 |
| --- | --- | --- |
| `memoryType` | 内存类型 | `HOST`、`HOST_PINNED` 或 `DEVICE`。具体 provider 是否支持由运行环境决定。 |
| `addr` | region 起始地址 | 必须是调用进程中有效的虚拟地址，不能为 0。 |
| `size` | region 总容量 | 必须覆盖所有引用它的 `KVBuffer` 地址范围。 |
| `deviceId` | 逻辑设备 ID | DEVICE 内存必须与实际分配设备一致；HOST 通常为 `-1`。 |
| `numaNode` | NUMA 节点 | 可选提示，不使用时为 `-1`。 |

Client 把 `HOST` 和 `HOST_PINNED` 都映射为 provider 的 host memory 类型。是否要求 pinned、是否支持普通 host memory，最终由 provider 返回结果决定。

## 3. 注册和 handle

```cpp
Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                       std::vector<RegisteredMemory>& registeredRegions);
```

成功时，输出与输入按下标一一对应：

```cpp
struct RegisteredMemory {
    MemoryRegion region;
    MRHandle handle;
    std::uint32_t tokenId;
};
```

- `handle` 是当前 KVClient 的 canonical handle，后续放入 `KVBuffer.buffer.handle`。
- `tokenId` 是跨 provider 绑定使用的 token，普通调用方不需要手工写入请求。
- handle 不能跨 KVClient 实例复用。
- 不能把 provider 的底层 handle 当作 Client handle 使用。

空输入合法，返回空输出。

注册失败时可能返回 `PARTIAL_FAILED`。Client 会尽力回滚已完成的注册或绑定；调用方不应使用失败调用返回的任何 handle。

## 4. KVBuffer 的地址范围

```cpp
struct KVBuffer {
    CacheKey key;
    Buffer buffer;
    std::uint32_t offset;
    std::optional<std::uint32_t> mrKey;
};
```

`buffer.region.addr` 和 `buffer.region.size` 描述本次 entry 使用的业务数据范围。它可以等于注册 region，也可以是注册 region 内的子范围，但必须满足：

```text
registered.addr <= entry.addr
entry.addr + entry.size <= registered.addr + registered.size
```

`buffer.handle` 必须指向覆盖该范围的注册 region。Client 会根据 handle 查找 token 并覆盖 `mrKey`，因此调用方不应设置或依赖输入的 `mrKey`。

`offset` 是 value 在 ASU 端逻辑 KV 数据中的偏移，不是相对于本地注册地址的偏移。

## 5. 对齐和长度

KV 协议对设备数据有以下约束：

- 数据长度按 512 字节对齐。
- `KVBuffer.offset` 按 512 字节对齐。
- 长度不能为 0。
- key 不能全为 0。
- 一个请求的 entry 数量不能超过 transport 和服务端协商的 batch 上限。

ASUStore 会把每个 tensor size 向上对齐到 512 字节，并据此重新计算 shard 和 block 的物理布局。直接调用 KVClient 时，调用方需要自己完成同样的对齐。

推荐工具函数：

```cpp
std::size_t AlignUp(std::size_t value)
{
    return (value + kv::kAlignmentBytes - 1) / kv::kAlignmentBytes * kv::kAlignmentBytes;
}
```

不要通过扩大 `region.size` 掩盖实际内存不足；region 声明的整个地址范围必须真实可访问。

## 6. 生命周期

安全顺序为：

1. 分配业务内存。
2. `RegisterRegions()`。
3. 构造引用注册 handle 的 `KVBuffer`。
4. 提交 Store/Load。
5. `Wait()` 直到任务完成。
6. 确认没有其他 queued 或 in-flight 任务引用 handle。
7. `UnregisterRegions()`。
8. 释放业务内存。

以下操作不安全：

- `StoreAsync()` 返回后立即复用或释放写入 buffer。
- `LoadAsync()` 返回后立即读取目标 buffer。
- 任务仍在队列中时注销 handle。
- 在一个 Client 注册，在另一个 Client 提交。
- 设备切换后继续使用属于其他逻辑设备的地址。

`StoreAsync()` 的 `eventHandle` 只负责等待上游写入完成，不延长 buffer 或 event 的生命周期。调用方仍需保持它们有效，直到同步和任务处理完成。

## 7. shared 和 independent provider

`SharedProviderMode` 控制多个 ASU transport 如何使用 provider：

| 模式 | 行为 | 适用考虑 |
| --- | --- | --- |
| `INDEPENDENT` | 每个 ASU transport 使用独立 provider；Client 另有一个业务内存注册 provider。 | 隔离性较强，但需要把同一注册 token 绑定到多个 provider。 |
| `SHARED` | 多个 ASU transport 共用一个 provider。 | 减少 provider 实例和重复绑定，但要求底层 provider 支持共享使用。 |

在 independent 模式下注册内存时：

1. 业务内存 provider 完成一次真实注册并取得 token。
2. Client 把该 token 绑定到每个 transport provider。
3. 每个 provider 得到自己的本地 handle。
4. Client 维护 canonical handle 到各本地 handle 的映射。

视图增加 ASU 时，新 provider 会绑定 Client 已记录的所有 region。视图删除 ASU不会立即破坏旧任务持有的 transport 快照。

## 8. 注销行为

```cpp
Status UnregisterRegions(const std::vector<MRHandle>& handles);
```

Client 按 provider 的逆序解除绑定，最后注销业务内存 provider 中的注册。无效 handle 和当前 Client 未跟踪的 handle会被忽略。

如果部分 provider 注销失败：

- API 返回第一个失败，并在 message 中补充 handle 数量。
- 仍然存在 provider 映射的 canonical handle 会保留在 Client 中。
- 调用方不能据此假定底层资源已经全部释放，应记录日志并进入受控清理流程。

## 9. ASUStore 的持久 region

ASUStore 从推理引擎接收 GPU KV cache 地址和大小，在 Setup 阶段批量注册为持久 region。每次 Dump/Load 时，Store 检查 tensor 地址是否完全位于某个已注册 region 内，并把对应 handle 传给 KVClient。

因此推理集成需要保证：

- GPU KV cache pool 在 ASUStore 生命周期内地址稳定。
- `gpu_kv_buffer_addrs` 与 `gpu_kv_buffer_sizes` 数量相同。
- 每个 task 中的 tensor 地址都属于已注册 pool。
- tensor layout、tensor size、shard size 和 block size 描述同一份实际内存布局。

出现 `ASU KV buffer is outside registered persistent regions` 时，应先检查引擎传入的 pool 地址和 task tensor 地址，而不是优先排查网络。
