# ASU 性能日志分析脚本

`analyze_asu_perf_logs.py` 用于解析 UCM 日志中的 `[ASU_PERF]` 事件，按
`client_task_id` 关联 AsuStore、Client、DHT 打散和 Transport 阶段，并输出每个任务的
耗时明细与汇总分位数。

## 环境要求

- Python 3.10 或更高版本。
- 不依赖第三方 Python 包。
- 输入文件必须包含启用 ASU 性能日志后产生的 `[ASU_PERF]` 日志。

## 基本用法

在仓库根目录执行：

```powershell
py -3 scripts/analyze_asu_perf_logs.py <日志文件>
```

Linux 环境可以使用：

```bash
python3 scripts/analyze_asu_perf_logs.py <日志文件>
```

脚本默认先向标准输出打印每个任务的 CSV 格式明细，再打印按操作类型汇总的统计数据。

同时分析多个日志文件：

```powershell
py -3 scripts/analyze_asu_perf_logs.py ucm-0.log ucm-1.log
```

将每任务明细保存为 CSV：

```powershell
py -3 scripts/analyze_asu_perf_logs.py ucm.log --csv asu_perf.csv
```

只查看汇总结果，不向标准输出打印每任务明细：

```powershell
py -3 scripts/analyze_asu_perf_logs.py ucm.log --no-tasks
```

也可以同时使用 `--no-tasks` 和 `--csv`：

```powershell
py -3 scripts/analyze_asu_perf_logs.py ucm.log --no-tasks --csv asu_perf.csv
```

## CSV 列顺序与加和关系

CSV 按“总时间、可加组成项、校验误差、重叠或派生指标”的顺序排列。主要的严格可加关系如下（由于各阶段分别取整为微秒，可能有少量误差）：

```text
store_total_us
  = store_submit_us + store_pre_wait_us + store_wait_us

client_total_us
  ≈ client_queue_us + client_scatter_us + client_transport_us
    + client_finalize_us

dht_scatter_us
  = dht_route_us + dht_reorder_us

critical_transport_total_us
  ≈ transport_queue_us + transport_prepare_us + transport_assign_us
    + transport_build_send_us + transport_send_setup_us + transport_send_us
    + transport_completion_wait_us
```

`client_timing_error_us` 和 `transport_timing_error_us` 是总时间减去组成项后的校验误差，不是额外耗时。`store_post_submit_us`、`client_dispatch_us`、`critical_transport_client_age_us` 和 `send_*` 是重叠、派生或统计指标，也不能再次加到对应总时间中。

## 每任务指标

| 字段 | 含义 |
| --- | --- |
| `source` | 日志文件路径。 |
| `pid` | 产生日志的进程 ID。 |
| `client_task_id` | ASU Client 任务 ID，用于关联各阶段。 |
| `trace_id` | AsuStore 在提交前生成的本地跟踪 ID。 |
| `op` | 操作类型，如 `query`、`batch_load`、`batch_store`。 |
| `items` | 本次任务包含的 key 或 KV entry 数量。 |
| `status` | 最终 ASU 状态码，`0` 表示成功。 |
| `store_total_us` | 从 AsuStore 调用异步提交之前，到 Wait 返回之后的总时间。 |
| `store_submit_us` | AsuStore 异步提交调用及其起始日志所用时间。 |
| `store_pre_wait_us` | Submit 返回后到调用方进入 Wait 前的时间，计算方式为 `store_total_us - store_submit_us - store_wait_us`。 |
| `store_wait_us` | AsuStore 实际调用 Client Wait 的阻塞时间。 |
| `store_post_submit_us` | `store_total_us - store_submit_us`，包含提交返回到调用 Wait 之间的间隔及 Wait 时间。 |
| `client_total_us` | ClientTask 从进入 Client 任务管理器到 Finalize 的总时间。 |
| `client_queue_us` | ClientTask 登记、入队并等待 Client worker 处理的时间。 |
| `client_scatter_us` | Client worker 构建全部 TransportTask 的时间。 |
| `client_transport_us` | DHT 打散结束到最后一个 TransportTask 完成的时间，包括分发和并行 Transport 执行。 |
| `client_finalize_us` | 最后一个 TransportTask 完成到 ClientTask Finalize 结束的时间。 |
| `client_timing_error_us` | Client 总时间减去四个可加阶段后的舍入误差，通常应接近 0。 |
| `client_dispatch_us` | Client 将所有 TransportTask 提交给 Transport 的调用时间；该指标包含在 `client_transport_us` 内，不能重复相加。 |
| `dht_scatter_us` | 完整 DHT 打散时间，包括路由、分组和 TransportTask 构建。 |
| `dht_route_us` | key 转换及 `RouteKeys` 路由所用时间。 |
| `dht_reorder_us` | `dht_scatter_us - dht_route_us`，用于观察分组和数据重排开销。 |
| `asu_count` | 本任务打散到的 ASU 数量。 |
| `transport_tasks` | 找到的 TransportTask 数量。 |
| `critical_transport_task_id` | 相对 Client 提交时间最晚完成的 TransportTask ID。多 ASU 并行时，后续 `transport_*` 阶段均取自这个临界 TransportTask。 |
| `critical_transport_total_us` | 临界 TransportTask 从提交到完成的总时间。 |
| `transport_queue_us` | 临界 TransportTask 的提交处理、入队及等待 Transport worker 的时间。 |
| `transport_prepare_us` | IO 切分、请求构建、buffer 分配及必要数据拷贝时间。 |
| `transport_assign_us` | 为子批次选择连接的时间。 |
| `transport_build_send_us` | 构建底层 SendIoBatch 数组的时间。 |
| `transport_send_setup_us` | 读取 Send 参数并进入底层 `TransProvider::Send` 前的准备时间。 |
| `transport_send_us` | 临界 TransportTask 的底层 `TransProvider::Send` 调用时间。 |
| `transport_completion_wait_us` | Send 返回到 TransportTask 完成的时间，包括设备执行、CQE/完成标志轮询、结果解包和资源释放。 |
| `transport_timing_error_us` | Transport 总时间减去上述阶段后的舍入误差，通常应接近 0。 |
| `critical_transport_client_age_us` | 临界 TransportTask 完成时，距离 ClientTask 提交已经经过的时间。 |
| `send_sum_us` | 所有 Transport Send 时间之和；并行发送时不能视为端到端耗时。 |
| `send_max_us` | 单个 Transport Send 的最大时间。 |
| `send_avg_us` | Transport Send 平均时间。 |
| `store_pre_wait_pct` | 调用 Wait 前时间占 Store 总时间的比例。 |
| `client_transport_pct` | Client Transport 阶段占 Client 总时间的比例。 |
| `transport_completion_wait_pct` | Send 后完成等待占临界 Transport 总时间的比例。 |
| `failed_sub_batches` | Send 返回失败的子批次数量。 |

空字段表示对应事件未出现在日志中，例如任务未经过 AsuStore，或者日志文件不完整。

## 汇总结果

`SUMMARY` 部分分别对全部任务和每种 `op` 输出以下统计值：

- `count`：有效样本数。
- `avg`：平均值。
- `p50`、`p95`、`p99`：对应分位数。
- `max`：最大值。

末尾的 `coverage` 显示解析到的任务数、同时具备 Store 与 DHT 数据的完整任务数，以及失败任务数。

## 注意事项

- 脚本使用“日志文件路径 + 进程 ID + Client 任务 ID”作为任务关联键。建议一次分析同一轮运行产生的日志。
- `[ASU_PERF]` 使用不限流日志。高并发下日志格式化和写入本身会产生开销，因此该数据更适合定位阶段占比。
- 测量绝对吞吐或延迟时，建议在关闭性能日志后再运行一轮基准作为对照。
- `send_sum_us` 可能包含并行 Transport 任务的重叠时间；分析端到端临界路径时优先参考 `send_max_us`。
- `store_post_submit_us` 可能包含业务线程在 Submit 返回后、调用 Wait 之前的停留时间，因此不等同于纯 ASU 执行时间。
- `client_dispatch_us` 与 Transport 各阶段用于进一步定位 `client_transport_us`，但由于多个 TransportTask 可能并行，不能把所有 TransportTask 的阶段时间直接相加到 Client 总时间。
