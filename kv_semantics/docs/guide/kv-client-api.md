# KVClient API 与行为约定

本文说明 `kv_client.h` 和 `kv_types.h` 中公开接口的使用契约。重点是调用方必须遵守、但仅看函数签名不容易发现的限制。

## 1. 对象生命周期

### CreateKvClient

```cpp
std::unique_ptr<KvClient> CreateKvClient();
```

创建对象不建立连接，也不启动业务 IO。返回对象必须先成功调用 `Init()`。

### Init

```cpp
Status Init(const KvClientConfig& config);
```

`Init()` 完成以下工作：

- 校验 Client 配置。
- 获取初始全局视图。
- 创建业务内存 provider。
- 为视图内的 ASU 创建 transport 和连接。
- 构建路由快照并启动 Client worker。

显式限制：

- `transportConfigs` 至少包含一项。
- `sharedProviderMode` 只能是 `INDEPENDENT` 或 `SHARED`。
- 视图引用的每个 ASU 都必须存在对应 `TransportConfig`。

隐含约定：

- 同一个对象只能成功初始化一次；重复调用返回 `RESOURCE_BUSY`。
- `Init()` 成功前不能提交任务或注册内存。
- 配置对象在 `Init()` 返回后可以释放，Client 保存的是副本。
- 初始化中任一 transport 创建失败，Client 不会发布不完整的路由快照。

### Shutdown

```cpp
Status Shutdown();
```

`Shutdown()` 首先停止接收新任务，然后等待 Client worker 处理已入队任务，并按 `defaultWaitTimeoutMs` drain 已跟踪任务，最后关闭 transport 和释放 provider 资源。

调用方应保证：

- 不再有线程提交新任务。
- 不再有线程使用同一个 Client 做内存注册或注销。
- 所有需要读取结果的任务已经 `Wait()`。

析构函数会调用 `Shutdown()`，但显式调用可以让业务获得关闭失败信息。

## 2. Key、Buffer 和公共类型

### CacheKey

```cpp
using CacheKey = std::array<std::byte, 8>;
```

key 固定为 8 字节，是二进制值，不是以 `\0` 结尾的字符串。全零 key 会在协议校验阶段被拒绝。路由使用完整 8 字节，调用方必须保证同一逻辑对象始终生成相同 key。

### KVBuffer

```cpp
struct KVBuffer {
    CacheKey key;
    Buffer buffer;
    std::uint32_t offset;
    std::optional<std::uint32_t> mrKey;
};
```

- `buffer.region` 描述业务内存地址、容量、内存类型和设备。
- `buffer.handle` 必须来自当前 Client 的 `RegisterRegions()`。
- `offset` 是该 entry 在 ASU 端 KV value 中的偏移。
- `mrKey` 由 Client 根据 handle 填充；调用方写入的值会被忽略。

设备 buffer 的长度和 offset 必须满足 512 字节对齐。更完整的规则见 [KVClient 内存与 Buffer 约定](kv-client-memory.md)。

## 3. 数据操作

所有数据操作都是异步提交接口。返回 `Status::OK()` 仅表示任务已进入 Client 队列，不表示 ASU IO 成功。

提交失败时，调用方不得使用输出 `taskId`；常见情况下它会被设置为 `kInvalidTaskId`。

### QueryAsync

```cpp
Status QueryAsync(const std::vector<CacheKey>& keys, TaskId& taskId);
```

查询每个 key 是否存在。完成后：

- `TaskResult.queryResult` 必须有值。
- `queryResult.exists.size()` 与输入 key 数量相同。
- `exists[i]` 对应输入 `keys[i]`，即使这些 key 被路由到不同 ASU。
- `prefixHitKeys` 是从 `keys[0]` 开始连续命中的 key 数量；中间第一次 miss 后停止计数。

空 key 列表是合法输入，会生成一个可完成的空任务。

### LoadAsync

```cpp
Status LoadAsync(const std::vector<KVBuffer>& entries, TaskId& taskId);
```

从 ASU 读取 value 到调用方提供的业务 buffer。

- entry 可以被路由和拆分到多个 ASU。
- 完成后的 `entryStatus` 按原输入顺序排列。
- Client 不拥有业务 buffer；任务完成前地址、注册状态和设备上下文必须有效。

协议选择由 Client 完成：entry 数量大于 1 时使用 batch load；只有一个 entry 时由 `KvClientConfig::useBatchIoForSingleKey` 决定使用 batch 还是 single 协议。

### StoreAsync

```cpp
Status StoreAsync(const std::vector<KVBuffer>& entries,
                  TaskId& taskId,
                  std::uintptr_t eventHandle = 0);
```

把业务 buffer 中的数据写入 ASU。协议选择规则与 `LoadAsync()` 相同。

当 `eventHandle != 0` 时，Client worker 会在拆分和发送任务前同步该事件。其用途是确保上游设备计算已经写完 buffer。调用方必须保证 event 在同步完成前有效，并且属于 Client 配置的设备运行时。

事件同步发生在 Client worker 线程中，因此一个长期未完成的事件会阻塞后续 Client 队列任务。

### DeleteAsync

```cpp
Status DeleteAsync(const std::vector<CacheKey>& keys, TaskId& taskId);
```

删除输入 key。结果的 `entryStatus` 与输入 key 一一对应。空列表合法，并返回可完成的空任务。

## 4. 任务模型

### TaskId

`TaskId` 由 Client 在提交时生成，`0` 是无效值。TaskId 只在创建它的 Client 对象中有效，不能跨 Client 或跨进程使用。

### Check

```cpp
bool Check(TaskId taskId);
```

返回值含义：

- `false`：任务仍在执行。
- `true`：任务已完成，或者该 TaskId 已不存在。

因此 `Check()` 不能区分“成功完成”“失败完成”和“任务不存在”。它只适合轮询完成状态，最终结果必须通过 `Wait()` 获取。

`Check()` 不消费任务，任务完成后仍可调用一次 `Wait()`。

### Wait

```cpp
Status Wait(TaskId taskId, std::uint64_t timeoutMs, TaskResult& result);
```

- `timeoutMs == 0` 时使用 `KvClientConfig::defaultWaitTimeoutMs`。
- 返回值是任务最终状态；等待超时或任务不存在时分别为 `TIMEOUT`、`TASK_NOT_FOUND`。
- `result.status` 与返回值保持一致，便于调用方把状态和逐项结果作为一个对象继续传递。
- `result.entryStatus` 保存每个输入项的结果。
- Query 额外通过 `result.queryResult` 返回命中信息。

最重要的隐含约定：`Wait()` 是消费操作。无论任务成功、失败还是等待超时，调用结束后 TaskId 都会从 Client 中移除。不能对同一个 TaskId 再次等待，超时后也无法重新取得最终结果。

推荐写法：

```cpp
kv::TaskResult result;
auto status = client->Wait(taskId, timeoutMs, result);
if (!status.ok()) {
    // 超时、任务不存在或任务执行失败；taskId 已被消费。
    // PARTIAL_FAILED 时继续查看 result.entryStatus。
    return status;
}
```

## 5. 聚合结果和部分失败

一个 Client task 可能按路由拆成多个 transport task。KVClient 等待所有已成功派发的 transport task 完成，再生成最终结果。

结果规则：

- 所有 transport task 成功：`TaskResult.status == OK`。
- 任一 transport task 失败：总体通常为 `PARTIAL_FAILED`。
- `entryStatus` 保持调用方原始输入顺序，可用于定位具体失败项。
- 一个 transport task 派发失败后，尚未派发到其他 ASU 的项会标记为 `CANCELED`；已派发项仍等待完成。
- Query 的失败项在 `exists` 中保留默认 miss 值，不能只看 `exists` 判断请求是否成功。

调用方处理 Query 时应检查：

1. `Wait()` 返回值；它与 `TaskResult.status` 一致。
2. 失败时逐项检查 `entryStatus`。
3. 总体成功时再使用 `queryResult`。

## 6. 内存注册 API

### RegisterRegions

```cpp
Status RegisterRegions(const std::vector<MemoryRegion>& regions,
                       std::vector<RegisteredMemory>& registeredRegions);
```

成功时输出数量与输入数量一致。每个结果包含：

- 原始 region。
- Client 级 canonical handle。
- provider 使用的 token ID。

在 independent provider 模式下，Client 先在业务内存 provider 注册，再把 token 绑定到每个 ASU provider。任一步失败都会返回失败，并尽力回滚本次注册。

空输入合法，返回空结果。

### UnregisterRegions

```cpp
Status UnregisterRegions(const std::vector<MRHandle>& handles);
```

Client 会先从各 provider 解除绑定，再注销业务内存 provider 中的注册。无效 handle 和当前 Client 未跟踪的 handle 会被忽略。

调用方必须保证 queued 或 in-flight 任务不再引用这些 handle。API 不会替调用方等待相关任务完成。

## 7. 路由和视图刷新

每个提交任务持有提交时的不可变视图快照，因此刷新不会改变已经入队任务的目的 ASU。

当前默认策略会在连接错误、IO 错误和超时等可刷新错误后异步请求新视图：

- 触发错误的当前任务不会自动重试。
- 后台刷新成功后，后续任务使用新快照。
- 新视图 epoch 不大于当前 epoch 时不会发布。
- 新快照构建失败时继续使用旧快照。
- 被移除 ASU 的 transport 会保留到旧任务不再使用，再在关闭阶段释放。

调用方需要自行决定业务重试策略，并确保重试操作是幂等的。

### 能力缺口

当前未实现按 kvns-id 区分的路由，也未完整适配 M800 路由地址二级列表与 M900 一级列表。初始化只按一份全局视图创建 transport，未构建多 KVNS 视图的去重并集，详见架构文档第 9 节。

当前 EID 由用户显式写入配置文件（本地设备 EID 和远端 ASU EID）；这属于当前实现的配置方式。未来需要改为自动发现 EID，不再将 EID 写入配置文件；该自动发现能力尚未实现。

### 连接管理行为约定

- ConnectionManager 按 endpoint 管理连接组，支持轮询和最小负载选择策略；关闭中不再选择连接。
- 错误按 channel 累计，成功清零；达到 `max_error_count` 后进入 DRAINING，不再接受新 IO。
- 后台恢复线程向原 endpoint 创建替代连接，失败则保留待恢复项继续尝试；恢复连接不会重放失败任务，也不等于刷新视图。
- Shutdown 停止恢复线程并释放连接组；调用方应先完成任务等待。
- 视图刷新按 ASU ID 复用已有 transport；同一 ASU ID 的 endpoint 改变不会自动重建已有 transport，不能依赖 epoch 增长完成地址切换。

### 其他模块行为约定

- Router 和任务聚合保存输入下标，跨 ASU 完成顺序不保证，结果按输入顺序恢复。
- Transport 可继续拆分协议子批次，业务 batch size 不等于单次网络发送 entry 数。
- Provider 的协议、设备内存和恢复能力依赖具体实现，FAKE 不能替代真实设备网络验证。
- 新 transport 的已有 region 绑定成功后才发布视图；调用方负责 buffer 和事件生命周期。

## 8. 并发使用边界

实现内部对任务表、提交队列和内存状态做了同步，但调用方仍应遵守以下边界：

- 由一个控制线程负责 `Init()` 和 `Shutdown()`。
- 可以从多个业务线程提交异步任务；队列满时会返回 `RESOURCE_BUSY`。
- 同一个 TaskId 只允许一个线程调用 `Wait()`。
- 不要让 `Check()`、`Wait()` 和 `Shutdown()` 对同一任务形成未定义的业务竞争。
- 内存注销应由统一的生命周期管理者执行，并发生在所有引用任务完成之后。

## 9. 状态码分类

| 类别 | 典型状态码 | 调用方处理建议 |
| --- | --- | --- |
| 参数/生命周期 | `INVALID_ARGUMENT`、`NOT_INITIALIZED`、`TASK_NOT_FOUND` | 修正调用顺序或参数，不应盲目重试。 |
| 资源压力 | `RESOURCE_BUSY`、`IN_PROGRESS` | 退避后重试，检查队列和 inflight 配置。 |
| 传输问题 | `CONNECTION_ERROR`、`IO_ERROR`、`TIMEOUT` | 当前请求不会自动重试；结合幂等性决定业务重试。 |
| 内存问题 | `BUFFER_NOT_REGISTERED`、`BUFFER_NOT_SUPPORTED` | 检查注册、地址范围、内存类型和对齐。 |
| 聚合失败 | `PARTIAL_FAILED`、`CANCELED` | 查看 `entryStatus`，区分已执行和未派发项。 |
| 服务端结果 | `ENTRY_*`、`CQE_*` | 保留原始状态，按具体协议结果判断是否可重试。 |

完整排查步骤见 [KVClient 排障指南](kv-client-troubleshooting.md)。
