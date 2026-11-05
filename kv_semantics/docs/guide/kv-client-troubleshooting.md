# KVClient 排障指南

排障时先确认失败发生在哪一层：配置加载、Client 初始化、Transport 连接、业务内存、异步任务，还是 ASUStore/推理集成。不要只根据最终的 `PARTIAL_FAILED` 判断根因，应同时查看 status message、逐 entry 状态和日志中的 node ID。

## 1. 建议采集的信息

问题现场至少保留：

- UCM commit、构建类型和 provider 构建开关。
- `.conf` 或 `.yaml` 配置，敏感地址可脱敏但保留列表长度和对应关系。
- Client、transport 和 ASU 服务端完整日志。
- `TaskResult.status`、`entryStatus` 及 status message。
- provider、设备 ID、ASU ID、地址、端口和 namespace。
- 请求 entry 数、单 entry 字节数、并发和 timeout。
- 问题是否可用单 ASU、单 entry 复现。

调试日志可按调用路径分别打开：

```bash
export KV_LOGGER_LEVEL=debug   # KVClient / Transport
export UC_LOGGER_LEVEL=debug   # UCM / ASUStore
```

仅在需要检查子批次切分时打开 trace，避免长期产生大量日志：

```bash
export ASU_TRACE=1
```

## 2. kv-test 找不到动态库

### 现象

- `dlopen` 失败。
- 找不到 `libkv_client.so` 或 `libkv_transport.so`。

### 检查

1. 确认已经构建 `kv_client`、`kv_transport` 和 `kv-test`。
2. 从仓库根目录执行 `source kv_semantics/kv_test/set_kvtest_env.sh`。
3. 检查 `build-kv-test/src` 和 `build-kv-test/kv_test` 下的产物。
4. 必要时在 `.conf` 中设置：

```ini
asu.client_library_path=/absolute/path/to/libkv_client.so
asu.transport_library_path=/absolute/path/to/libkv_transport.so
```

### 常见原因

- 只执行脚本而没有 `source`，环境变量没有留在当前 shell。
- `BUILD_DIR` 与实际构建目录不一致。
- RPATH 或运行时依赖缺失。

## 3. provider 配置正确但初始化失败

### 现象

- provider unsupported。
- 创建 provider 失败。
- AIV/AICPU 相关符号或依赖缺失。

### 检查

运行时配置必须与构建开关一致：

| 配置 | 构建要求 |
| --- | --- |
| `FAKE` / `fake` | `BUILD_KV_CLIENT_PROVIDER_FAKE=ON` |
| `AIV` / `aiv` | `BUILD_KV_CLIENT_PROVIDER_AIV=ON`，并能找到 `libumc.a` 及其运行时依赖。 |
| `AICPU` / `aicpu` | `BUILD_KV_CLIENT_PROVIDER_AICPU=ON`，并能找到 HCOMM 头文件和库。 |

先用 `kv-test config check` 验证解析，再用 `kv-test connect` 验证初始化。不要直接从完整推理服务开始排查。

## 4. Init 返回 NOT_FOUND 或找不到 ASU

### 检查

- `transport.asu_ids` 是否包含视图中的全部 ASU。
- 每个 ASU 是否有匹配的 `asu_info.<id>` 或 YAML 地址。
- `view.config_path` 是否相对于当前工作目录可访问。
- view 文件中的 `asuIds`、`viewEpoch` 是否正确。
- YAML 中 `asu_ids`、`asu_ips`、`asu_ports` 数量是否相同。

视图引用缺少 TransportConfig 的 ASU 时，初始化会失败，不会发布部分可用的 snapshot。

## 5. 多 ASU 路由后数据找不到

### 现象

- store 成功但 query/load miss。
- 不同 worker 对同一个 key 命中结果不同。

### 检查

- 所有 worker 的 ASU membership 是否一致。
- `asu_ids` 的值是否一致；顺序不是关键，ID 集合才是关键。
- 路由类型和 Ring Hash/Maglev 参数是否一致。
- key 生成是否稳定、是否始终为相同 8 字节。
- `kv_ns_id` 是否与服务端一致。
- view epoch 更新后，ASU membership 是否发生变化。

路由配置不一致会让相同 key 落到不同 ASU，看起来像数据丢失，但实际是访问了不同节点或 namespace。

## 6. RESOURCE_BUSY

`RESOURCE_BUSY` 可能来自两个不同队列：

- `client task queue is full`：调大 `client.max_inflight_tasks` 或 `asu_client_max_inflight_tasks`，也要检查 Client worker 是否被 event 同步阻塞。
- `transport task queue is full`：调大 `transport.max_inflight_tasks` 或 `asu_transport_max_inflight_tasks`，并检查 provider 发送和完成速度。

不要只增大队列。持续堆积通常说明下游吞吐不足，盲目增大只会增加内存和尾延迟。调用方应对 `RESOURCE_BUSY` 做有上限的退避重试。

## 7. Wait TIMEOUT

### 重要行为

`Wait()` 超时后 TaskId 已被消费，不能再次 Wait。`Check()` 随后返回 `true` 也可能只是因为任务已不存在。

### 检查

- 区分 Client Wait timeout、transport task timeout 和 provider send timeout。
- 检查 completion worker 是否运行、CQE 是否到达。
- 检查 `completion_poll_spin_limit`；它不是 timeout，但极端设置会影响调度。
- 检查 ASU 端是否已接收请求。
- Store 带 event 时，检查 event 是否完成。

ASUStore 中：

- Dump/Load 使用 `asu_wait_timeout_ms`。
- Lookup 使用 `asu_query_timeout_ms`。
- Lookup 超时按 miss 返回，可能表现为缓存命中率突然下降。

## 8. PARTIAL_FAILED

### 现象

Wait 返回 `PARTIAL_FAILED`，并且 `TaskResult.status` 也是 `PARTIAL_FAILED`。

`Wait()` 的返回值就是任务最终状态；`TaskResult.status` 保存同一状态，`entryStatus` 提供逐项细节。

### 检查

1. 输出每个 `entryStatus[i]` 的 code 和 message。
2. 用原输入 `entries[i]` 或 `keys[i]` 对应具体业务项。
3. 在日志中查找 `client_task_id`、`transport_task_id` 和 `nodeId`。
4. 区分真实失败项与因前序派发失败产生的 `CANCELED`。

Query 失败时，`exists` 中失败位置可能保持为 0。不能把它直接解释为 key 不存在。

## 9. BUFFER_NOT_REGISTERED 或 buffer outside registered region

### 检查

- `KVBuffer.buffer.handle` 是否来自同一个 Client 的 `RegisterRegions()`。
- entry 地址范围是否完全位于注册 region 内。
- region 是否在任务完成前被注销。
- DEVICE region 的逻辑 `deviceId` 是否正确。
- ASUStore 的 GPU KV pool 地址是否稳定。
- `gpu_kv_buffer_addrs` 和 `gpu_kv_buffer_sizes` 是否一一对应。

出现 `ASU KV buffer is outside registered persistent regions` 时，错误发生在 Store 构造 KVBuffer 阶段，请优先检查引擎传入地址和 pool 注册范围，尚未进入网络发送。

## 10. 协议校验或 INVALID_ARGUMENT

常见原因：

- key 是全零 8 字节。
- buffer 地址或长度为 0。
- DEVICE buffer 长度不是 512 字节对齐。
- `offset` 不是 512 字节对齐。
- batch 数量超过有效上限。
- endpoint 端口超出 1 到 65535。
- AICPU 协议、地址类型或发送 timeout 非法。

Transport 初始化会读取服务端 capability，并取本地配置与服务端上限的较小值。应查看日志中的 `effective batch limits`，不要只看配置文件中的 batch 数值。

## 11. Store 成功但 Retrieve 一致性失败

### kv-test

- 确认 store 和 retrieve 使用相同的 `kv.key_prefix`、`kv.seed` 和 `kv.value_size`。
- FAKE provider 必须设置 `complete_immediately=false` 才会执行本地文件 IO。
- 检查两次命令是否使用同一 `fake_backend.path` 和 ASU ID。
- 检查 retrieve 是否发生在 store 的 Wait 完成之后。

### ASUStore

- 检查 namespace、tensor layout、tensor size、shard size 和 block size。
- 检查 Dump 是否带了未完成或错误设备的 prerequisite event。
- 检查相同 BlockId 是否稳定映射为相同 8 字节 key。

## 12. Query 命中异常

- `exists` 与输入 key 顺序一致，不与 ASU 完成顺序一致。
- `prefixHitKeys` 只计算从第一个 key 开始的连续命中。
- 第 0 个 key miss 时，即使后续全部命中，`prefixHitKeys` 也是 0。
- Query 部分失败时先看总体和逐项状态，再解释 exists。

ASUStore 的 `LookupOnPrefix()` 返回最后一个连续命中的下标，因此：

- `prefixHitKeys == 0` 转换为 `-1`。
- `prefixHitKeys == N` 转换为 `N - 1`。

## 13. 视图没有刷新或刷新后仍使用旧 ASU

默认会触发刷新的状态包括：

- `CONNECTION_ERROR`
- `IO_ERROR`
- `TIMEOUT`
- `NOT_FOUND`
- `BUFFER_NOT_REGISTERED`

检查：

- 后台日志是否出现 refresh 失败。
- 新 view 的 `viewEpoch` 是否大于当前值；相同或更旧 epoch 不会发布。
- 新 ASU 是否有 TransportConfig。
- 新 transport 是否能初始化并绑定已有业务内存。

刷新只影响后续提交。已经持有旧 snapshot 的任务继续访问旧 transport，这是设计行为。

## 14. Shutdown 卡住或耗时长

Shutdown 会停止接收任务、等待 Client worker 排空队列、drain 任务、关闭每个 transport，并处理仍在途的完成。可能的阻塞点包括：

- Client worker 正在等待 prerequisite event。
- 队列中仍有大量任务。
- provider Send 或完成轮询没有返回。
- transport 等待配置的 timeout 后才取消在途任务。
- view refresh 正在创建或关闭 transport。

业务应先停止生产请求，再逐个 Wait 已提交任务，最后调用 Shutdown。不要把 Shutdown 当作正常任务完成接口。

## 15. 推理服务启动失败

按以下顺序缩小范围：

1. `kv-test config check`：验证字段和动态库。
2. `kv-test connect`：验证 Client、provider 和 ASU 连接。
3. kv-test 单 key store/retrieve：验证协议和 namespace。
4. ASUStore 单 worker、单 ASU启动。
5. 发送短 prompt，确认 Dump/Load 日志。
6. 再扩展 TP/DP、多 ASU 和长上下文。

ASUStore 配置日志中至少确认：mode、ASU IDs、provider、namespace、device ID、tensor layout 和 tensor sizes。推理能够返回结果并不等于外部 KV 已命中；用相同 prompt 重复请求并结合 UCM 日志与指标确认。

## 16. 指标定位

优先关注：

- `kv_client_*_errors_total`：哪类 Client 提交持续失败。
- `kv_client_task_queue_duration_seconds`：Client worker 是否积压。
- `kv_client_task_e2e_duration_seconds`：调用端到任务完成总耗时。
- `kv_transport_task_send_call_duration_seconds`：provider Send 本身是否慢。
- `kv_transport_task_completion_duration_seconds`：Send 后等待完成是否慢。
- `kv_transport_task_completion_timeouts_total`：完成等待超时。
- `kv_transport_task_connection_errors_total`：连接错误。

如果 queue duration 高而 send/completion 正常，瓶颈更可能在 Client 排队；如果 send 快但 completion 慢，应检查 ASU 服务端和数据通路；如果只在特定 node ID 变慢，应按 ASU 节点拆分日志和指标。
