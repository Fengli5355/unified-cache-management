# KVClient 与 ASUStore 配置说明

KVClient 当前有两种配置入口：

- `kv-test` 使用 `key=value` 的 `.conf` 文件。
- ASUStore 使用 UCM 的 `.yaml` 文件，并把其中的 `asu_*` 字段转换为 `KvClientConfig`。

两种格式描述的是同一套 Client、路由、Transport 和 Provider 能力，但字段名和使用场景不同。

## 1. kv-test `.conf` 配置

示例文件为 `kv_semantics/kv_test/kv_test_aicpu.conf`。空行和以 `#` 开头的行会被忽略；格式不是 YAML。

```ini
key=value
```

相对路径按进程当前工作目录解析，不按 `.conf` 文件所在目录解析。仓库示例假定从仓库根目录运行。

### 1.1 Client 和视图

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `client.client_id` | 空 | Client 标识，用于日志和服务端识别。 |
| `client.max_inflight_tasks` | `1024` | Client 提交队列容量；满时提交返回 `RESOURCE_BUSY`。 |
| `client.use_batch_io_for_single_key` | `true` | 单 entry 的 Load/Store 是否仍使用 batch 协议。 |
| `client.asu_shared_provider` | `0` | `0` 为 independent，`1` 为 shared。 |
| `wait_timeout_ms` | `100` | 默认 Wait、Shutdown drain 和 transport 超时基础值。 |
| `view.config_path` | 空 | 本地视图文件。刷新时重新读取该文件。 |
| `view_service_addrs` | 空 | 外部 view service 地址列表；当前本地示例通常不用。 |

视图文件示例：

```ini
viewEpoch=1
viewId=1
asuIds=1,2
asu_info.1=protocol=TCP,local.comm_id=10.0.0.11,port=19001
asu_info.2=protocol=TCP,local.comm_id=10.0.0.12,port=19001
```

`viewEpoch` 只有增大时才会发布新快照。视图中的每个 ASU ID 都必须出现在 `transport.asu_ids` 中。

### 1.2 路由

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `client.hash_table.type` | `RING_HASH` | 支持范围限定为 `RING_HASH` 和 `MAGLEV`；affinity 参数可解析但不在支持范围内。 |
| `client.ring_hash.virtual_node_count` | `128` | Ring Hash 每个 ASU 的虚拟节点数。 |
| `client.maglev.table_size` | `65537` | Maglev 查找表大小。 |
| `client.contiguous_block_affinity.block_count` | `1` | 连续多少个 key 固定到同一 ASU。 |
| `client.contiguous_block_affinity.full_spread_type` | Ring Hash | affinity 之下使用的全分散路由。 |
| `client.batch_topk_affinity.top_k` | `1` | 每个 batch 限制到多少个候选 ASU。 |

`dynamic_adjust_enabled` 字段当前可以解析，但不应据此假定已经存在完整的动态调节控制面；使用前应通过对应测试验证实际行为。

### 1.3 Transport 和 Provider

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `transport.asu_ids` | 空 | Transport ASU ID 列表，至少一项。 |
| `transport.provider_type` | `AICPU` | `AICPU`、`AIV` 或 `FAKE`，必须与构建开关一致。 |
| `transport.device_id` | `-1` | provider 使用的本地逻辑设备 ID。 |
| `transport.max_inflight_tasks` | `1024` | 每个 transport 的最大并发任务数。 |
| `transport.max_error_count` | `2` | 连接连续错误阈值。 |
| `transport.completion_poll_spin_limit` | `16` | 连续空轮询多少次后让出 CPU。 |
| `transport.batch_load_io_num` | `110` | 单个 batch load 子批次最大 entry 数。 |
| `transport.batch_store_io_num` | `110` | 单个 batch store 子批次最大 entry 数。 |
| `transport.delete_io_num` | `254` | 单个 delete 子批次最大 key 数。 |
| `transport.query_io_num` | `256` | 单个 query 子批次最大 key 数。 |

`transport.*` 中未被通用解析器消费的字段会进入 provider attrs，例如 `kv_ns_id`、`dtype`、`dspec`、`lr`、`sc`、`kernel_count` 和 `quiet_count`。这些值需要与 ASU 服务端协议配置一致。

Endpoint 使用：

```ini
asu_info.1=protocol=UB,local.comm_id=<address>,port=19001
```

多 endpoint 用分号分隔。`asu_info.<id>` 中的 id 必须对应 `transport.asu_ids`。

当前 EID 由用户显式写入配置文件（本地设备 EID 和远端 ASU EID）；这属于当前实现的配置方式。未来需要改为自动发现 EID，不再将 EID 写入配置文件；该自动发现能力尚未实现。 `.conf` 中的 `transport.aicpu_local_eid.<logical_device_id>` 和 `asu_info.<id>`、视图文件中的远端 EID 均遵循此限制。

### 1.4 FAKE provider

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `fake_backend.path` | `./asu-fake-backend-store` | 本地数据目录，按 `asu-<id>` 分目录。 |
| `fake_backend.latency_us` | `1000` | 模拟完成延迟，单位微秒。 |
| `fake_backend.worker_threads` | `4` | 后端工作线程数，必须大于 0。 |
| `fake_backend.complete_immediately` | `false` | 为 `true` 时跳过文件 IO，读取和查询按 miss 处理。 |

### 1.5 kv-test 数据和压力配置

| 字段 | 说明 |
| --- | --- |
| `kv.key_prefix` | 使用 `--count` 生成 key 时的前缀。 |
| `kv.seed` | 确定性 value 生成种子。 |
| `kv.value_size` | 普通 store/retrieve 的 value 字节数。 |
| `kv.count` | 未在 CLI 指定 key 时的默认数量。 |
| `limits.memory_max_bytes` | value payload / bench 可复用 buffer 池字节上限（非进程总内存），默认 4 GiB。 |
| `bench.io_size` | bench 单 entry 的逻辑 IO 大小。 |
| `bench.io_interval_us` | 相邻批次提交起点的最小间隔，单位微秒；仍等待上一批完成，不追赶补发。 |
| `bench.concurrency` | 原异步设计的 Wait 线程数，不是 IO 并发数。当前同步 bench 不创建 Wait 线程池，逐批提交并等待，实际在途批次为 1；该值要求大于 0，并参与 key 池大小计算。 |
| `bench.duration_sec` | 按时间运行的测量时长。 |
| `bench.io_count` | 大于 0 时按固定 entry 数运行（非 batch 数），替代 duration。 |
| `bench.warmup_sec` | 预热时长。 |
| `bench.read_ratio` / `bench.write_ratio` | mix 模式读写比例，两者应合计 100。 |
| `bench.batch_size` | 每个 batch 的 entry 数。 |

CLI 同名参数覆盖 `.conf` 中的值。

### 1.6 输出、动态库和指标

| 字段 | 说明 |
| --- | --- |
| `asu.client_library_path` | `libkv_client.so` 的显式路径。 |
| `asu.transport_library_path` | `libkv_transport.so` 的显式路径。 |
| `output.path` | 结果输出目录。 |
| `output.realtime_file_max_bytes` | 实时结果文件大小上限。 |
| `metrics.enabled` | 是否启动 kv-test 独立 Prometheus endpoint。 |
| `metrics.config_path` | 指标定义文件路径。 |
| `metrics.listen_address` / `metrics.port` / `metrics.path` | 监听地址、端口和 HTTP path。 |
| `metrics.source` / `metrics.model_name` / `metrics.worker_id` | 指标标签。 |
| `metrics.aggregation_interval_ms` | 聚合周期，启用指标时必须大于 0。 |

## 2. ASUStore `.yaml` 配置

ASUStore 必须位于 `UcmPipelineStore` 的 `ucm_connector_config` 中：

```yaml
ucm_connectors:
  - ucm_connector_name: "UcmPipelineStore"
    ucm_connector_config:
      store_pipeline: "ASU"
      asu_mode: "client"
      asu_ids: [1]
      asu_ips: ["10.0.0.11"]
      asu_ports: [19001]
      asu_trans_provider_backend: "aiv"
      kv_ns_ids: [100]

enable_event_sync: true
use_layerwise: false
persist_token_threshold: 0
```

### 2.1 ASU 身份和连接

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `asu_mode` | `client` | `client` 使用 KVClient 路由；`transport` 只允许一个 ASU。 |
| `asu_client_id` | `ucm-asu-store` | 当前推理 worker 的 Client 标识。 |
| `asu_config_path` | 空 | 可选 KVClient `.conf`；设置后从文件加载 Client 配置。FAKE 不支持。 |
| `asu_view_service_addrs` | 空 | 外部视图服务地址。 |
| `asu_ids` | 空 | ASU ID 列表；未使用 `asu_config_path` 时必填、非负且不能重复。 |
| `asu_ips` | 空 | ASU 地址，非空时数量必须与 `asu_ids` 相同。 |
| `asu_ports` | 空 | ASU 端口，数量与 `asu_ips` 相同，范围 1 到 65535。 |
| `asu_local_ip` | 空 | 本地通信地址；AICPU 未配置地址数组时可使用。 |
| `asu_name_prefix` | `asu` | 生成 transport node name 的前缀。 |
| `kv_ns_ids` | 空 | ASU KV namespace。普通模型恰好 1 个，FAWA 恰好 2 个。 |

当设置 `asu_config_path` 时，KVClient 的 Client/Transport/路由配置由 `.conf` 加载，不与 YAML 生成结果做字段级合并；ASUStore 自身的 query、tensor layout 和 Store 生命周期字段仍来自 YAML。

### 2.2 Provider

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `asu_trans_provider_backend` | `aicpu` | `aicpu`、`aiv` 或 `fake`。 |
| `asu_shared_provider` | `0` | `0` 独立 provider，`1` 共享 provider。 |
| `asu_sc` | `true` | 透传给 SQE/provider 的 `sc` 属性。 |
| `asu_max_error_count` | `2` | 非 0 的 uint32 错误阈值。 |
| `asu_completion_poll_spin_limit` | `16` | 完成轮询空转阈值，必须大于 0。 |
| `asu_max_inflight_bytes` | `1 GiB` | 每个 transport 最大在途字节数。 |

AICPU 专用字段：

| 字段 | 说明 |
| --- | --- |
| `asu_aicpu_hcomm_protocol` | `ubg` 或 `ubc_ctp`；`ub`、`ub_ctp` 是 CTP 别名。 |
| `asu_aicpu_local_addrs` | 按 ACL 逻辑设备 ID 排列的本地 IP/EID。UBG 要求 EID。 |
| `asu_aicpu_send_timeout_ms` | HCOMM/HIXL 发送等待，范围 `[1, UINT32_MAX]`。 |

当前 EID 由用户显式写入配置文件（本地设备 EID 和远端 ASU EID）；这属于当前实现的配置方式。未来需要改为自动发现 EID，不再将 EID 写入配置文件；该自动发现能力尚未实现。 YAML 中的 `asu_aicpu_local_addrs` 和 `asu_ips` 当前需显式填写相应 EID。

FAKE 专用字段：

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `asu_fake_backend_path` | 空 | 本地存储根目录。 |
| `asu_fake_backend_latency_ms` | `1` | 模拟延迟，ASUStore 会转换为微秒传给 provider。 |
| `asu_fake_backend_worker_threads` | `4` | 必须大于 0。 |
| `asu_fake_backend_complete_immediately` | `false` | 跳过文件 IO，仅验证完成链路。 |

### 2.3 超时和并发

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `asu_wait_timeout_ms` | `100` | Dump/Load Wait、Client 默认 Wait 和 transport timeout，必须大于 0。 |
| `asu_query_timeout_ms` | `500` | Lookup 使用的独立查询超时，必须大于 0。 |
| `asu_client_max_inflight_tasks` | `1024` | Client 任务队列容量。 |
| `asu_transport_max_inflight_tasks` | `1024` | 每个 ASU transport 在途任务上限。 |
| `asu_use_batch_io_for_single_key` | `true` | 单 entry 是否使用 batch 协议。 |

Query 超时在 ASUStore 的 Lookup 接口中按 miss 处理，因此超时配置过小可能表现为命中率下降，而不是直接终止推理。

### 2.4 路由

| YAML 字段 | 转换后的 Client 字段 |
| --- | --- |
| `asu_router_type` | `hash_table.type` |
| `asu_ring_hash_virtual_node_count` | `ring_hash.virtual_node_count` |
| `asu_maglev_table_size` | `maglev.table_size` |
| `asu_contiguous_block_affinity_block_count` | `contiguous_block_affinity.block_count` |
| `asu_contiguous_block_affinity_full_spread_type` | `contiguous_block_affinity.full_spread_type` |
| `asu_batch_topk_affinity_top_k` | `batch_topk_affinity.top_k` |

多 ASU 环境必须让所有推理 worker 使用一致的路由参数，否则同一个 key 可能访问不同 ASU。

### 2.5 Tensor 和设备布局

以下字段通常由 UCMConnector 根据模型和 KV cache pool 注入，不建议用户在静态 YAML 中手工填写：

- `unique_id`
- `gpu_kv_buffer_addrs`
- `gpu_kv_buffer_sizes`
- `device_id`
- `tensor_size` 或 `tensor_size_list`
- `shard_size`
- `block_size`
- `tensor_layout`

支持的 `tensor_layout` 为 `mla`、`gqa` 和 `hma`。ASUStore 会把 tensor size 向上对齐到 512 字节，并保持 block 中的 shard 数量不变。

### 2.6 UCM/Store 集成字段

这些字段不属于 KVClient，但会影响 ASUStore 的调用方式：

| 位置 | 字段 | 说明 |
| --- | --- | --- |
| YAML 顶层 | `enable_event_sync` | 启用上游设备事件同步；Store 会把 prerequisite event 传给 `StoreAsync()`。 |
| YAML 顶层 | `use_layerwise` | 是否按层加载和保存 KV。必须与模型布局和 ASUStore tensor 配置匹配。 |
| YAML 顶层 | `persist_token_threshold` | 请求 token 数低于阈值时跳过外部 KV 持久化。 |
| `ucm_connector_config` 内 | `enable_metrics` | Store 级 ASU/KV metrics backend 安装开关，默认 `true`。backend 是进程级的。 |

## 3. 配置选择建议

- 开发验证：FAKE provider、单 ASU、Ring Hash。
- 首次真实联调：单 ASU、真实 provider、较大的 timeout、低并发。
- 多 ASU：先保证 ID/IP/port 数量一致，再统一路由参数。
- 推理压测：分别调节 Client queue 和每个 transport inflight，避免把两个瓶颈混为一谈。
- AICPU：优先使用 `examples/ucm_config_asu_aicpu.yaml`，按逻辑设备逐项替换本地地址。
- AIV：优先使用 `examples/ucm_config_asu.yaml`，确认 namespace 和服务端一致。
