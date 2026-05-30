# kv-test 命令行工具规范

## 功能摘要

`kv-test` 是面向 ASU KV 接口的命令行测试工具。它负责完成建链、Store、Retrieve、Delete、Exist、BatchStore、BatchRetrieve、掉电前后数据一致性测试和性能测试。工具从命令行参数和配置文件读取输入，向 ASU KV 后端下发请求，并输出文本、JSON 和 CSV 格式的测试结果。

当前文档定义工具的目标行为，未实现或需实测确认的内容使用中文方括号标记。

## 使用场景

适用场景：

- 验证 ASU KV 建链流程是否可用。
- 下发单条 KV Store、Retrieve、Delete、Exist 请求。
- 下发批量 BatchStore、BatchRetrieve 请求。
- 验证 ASU 掉电前写入的数据在重新上电后是否一致。
- 执行 KV 读写性能测试，并统计 IOPS、带宽、时延和 QOS 指标。

不适用场景：

- 不用于 CI 或自动化平台接入；该能力暂不考虑。
- 不用于存储侧执行快路径操作。工具只提供测试入口。
- 不用于执行 ASU 物理下电或上电动作。工具只负责 `prepare` 和 `verify` 两个阶段，下电上电动作由测试人员或外部系统完成。

## 术语

工具命名必须使用以下术语：

| 工具术语 | 含义 | 底层接口映射 |
| --- | --- | --- |
| `Store` | 写入 KV 数据。 | Store |
| `Retrieve` | 读取 KV 数据。 | Load |
| `Delete` | 删除 KV 数据。 | Delete |
| `Exist` | 查询 key 是否存在。 | Query/Exist |
| `BatchStore` | 批量写入 KV 数据。 | BatchStore |
| `BatchRetrieve` | 批量读取 KV 数据。 | BatchLoad |

## 命令格式

```bash
kv-test <命令> [参数]
```

当前实现说明：

- `kv-test --help` 和 `kv-test -h` 打印命令帮助并成功退出。
- 设置 `KV_TEST_CONFIG` 后，`--configpath <path>` 可省略。
- 配置路径优先级为：先使用 `--configpath`，再使用 `KV_TEST_CONFIG`。
- 如果既未设置 `--configpath`，也未设置 `KV_TEST_CONFIG`，工具输出错误信息并退出。
- 每次调用都会打印终端结果行。成功命令打印 `kv-test: succeeded ...`；失败命令打印 `kv-test: failed ...`。
- 当前实现使用已有 ASU 客户端 key-value 配置格式，不使用 YAML。配置示例位于 `ucm/transport/kv/kv-test/asu_kv_test.conf`。

支持的命令：

| 命令 | 功能 |
| --- | --- |
| `connect` | 只执行建链流程。 |
| `store` | 下发单条或多条 Store 请求。 |
| `retrieve` | 下发单条或多条 Retrieve 请求。 |
| `delete` | 下发单条或多条 Delete 请求。 |
| `exist` | 下发单条或多条 Exist 请求。 |
| `batch-store` | 下发 BatchStore 请求。 |
| `batch-retrieve` | 下发 BatchRetrieve 请求。 |
| `power-cycle prepare` | 掉电一致性测试的写入阶段。 |
| `power-cycle verify` | 掉电一致性测试的上电后校验阶段。 |
| `bench` | 执行性能测试。 |

示例：

```bash
kv-test store --keys key1,key2,key3 --configpath ./config/asu_kv_test.yaml
kv-test retrieve --keys key1,key2,key3 --configpath ./config/asu_kv_test.yaml --check
kv-test delete --keys key1,key2,key3 --configpath ./config/asu_kv_test.yaml --check
kv-test exist --keys key1,key2,key3 --configpath ./config/asu_kv_test.yaml
kv-test bench --op batch-retrieve --configpath ./config/asu_kv_test.yaml
```

## 输入

### 通用参数

| 参数 | 类型 | 必填 | 默认值 | 允许为空 | 说明 |
| --- | --- | --- | --- | --- | --- |
| `--configpath <path>` | 字符串 | 是 | 无 | 否 | 配置文件路径。 |
| `--key <key>` | 字符串 | 否 | 无 | 否 | 单个 key。与 `--keys`、`--count` 互斥。 |
| `--keys <k1,k2,...>` | 字符串列表 | 否 | 无 | 否 | 多个 key，使用英文逗号分隔。与 `--key`、`--count` 互斥。 |
| `--count <n>` | 无符号 64 位整数 | 否 | 配置文件 `kv.count` | 否 | 使用 seed 生成的 key 数量。 |
| `--seed <n>` | 无符号 64 位整数 | 否 | 配置文件 `kv.seed` | 否 | key/value 数据生成随机数种子。 |
| `--value-size <bytes>` | 无符号 64 位整数 | 否 | 配置文件 `kv.value_size` | 否 | 单个 value 大小，单位为字节。 |
| `--batch-size <n>` | 无符号 32 位整数 | 否 | 配置文件对应批量大小 | 否 | 单个批量请求包含的最大条目数，仅支持 `batch-store`/`batch-retrieve`。 |
| `--check` | 布尔值 | 否 | `false` | 是 | 开启一致性检查。 |
| `--timeout <ms>` | 无符号 64 位整数 | 否 | 配置文件 `connection.timeout_ms` | 否 | 单请求或单批请求超时时间，单位 ms。 |
| `--output <path>` | 字符串 | 否 | 配置文件 `output.path` | 否 | 结果输出目录。 |
| `--verbose` | 布尔值 | 否 | `false` | 是 | 输出详细日志。 |

输入优先级：

1. 命令行参数优先级高于配置文件。
2. `--key`、`--keys`、`--count` 三者最多指定一个。
3. 指定 `--count` 时，必须能从命令行或配置文件读取 `seed`、`value_size` 和 `key_prefix`。

### 性能测试参数

| 参数 | 类型 | 必填 | 默认值 | 枚举值或范围 | 说明 |
| --- | --- | --- | --- | --- | --- |
| `--op <op>` | 枚举 | 是 | 无 | `store`、`retrieve`、`batch-store`、`batch-retrieve`、`mix` | 压测操作类型。 |
| `--io-size <bytes>` | 无符号 64 位整数 | 否 | 配置文件 `bench.io_size` | `>0` | 单个 KV value 大小。 |
| `--concurrency <n>` | 无符号 32 位整数 | 否 | 配置文件 `bench.concurrency` | `>0` | 并发 IO 数。 |
| `--duration <sec>` | 无符号 64 位整数 | 否 | 配置文件 `bench.duration_sec` | `>0` | 统计阶段时长。 |
| `--warmup <sec>` | 无符号 64 位整数 | 否 | 配置文件 `bench.warmup_sec` | `>=0` | 预热时长，预热数据不计入性能结果。 |
| `--read-ratio <n>` | 无符号 32 位整数 | 否 | 配置文件 `bench.read_ratio` | `0..100` | 读比例。 |
| `--write-ratio <n>` | 无符号 32 位整数 | 否 | 配置文件 `bench.write_ratio` | `0..100` | 写比例。 |

约束：

- `--op mix` 时，`read_ratio + write_ratio` 必须等于 `100`。
- `--op retrieve` 和 `--op batch-retrieve` 时，读写比例无需提供，自动使用`read_ratio` 为 `100`，`write_ratio` 为 `0`。
- `--op store` 和 `--op batch-store` 时，读写比例无需提供，自动使用`read_ratio` 为 `0`，`write_ratio` 为 `100`。
- 预热阶段不执行一致性检查；指定 `--check` 时，工具在预热完成后、统计开始前执行一次一致性采样检查。

## 配置文件

当前实现：

- 配置文件使用 ASU 客户端 key-value 格式。
- 设置一次 `KV_TEST_CONFIG=/abs/path/to/asu_kv_test.conf` 后，后续命令可复用同一配置。
- 单次命令仍可通过 `--configpath <path>` 覆盖环境变量。
- 示例文件为 `ucm/transport/kv/kv-test/asu_kv_test.conf`。

以下 YAML 片段为规划配置结构示例；当前实现以 ASU 客户端 key-value 配置格式为准。配置应能表达建链参数、KV 生成参数、批量限制、性能参数和输出参数。

```yaml
connection:
  target: "[ASU服务地址或设备标识]"
  protocol: "[UB|ROCE|TCP]"
  timeout_ms: 30000
  retry: 3
  extra: "[ASU建链配置字段]"

kv:
  key_prefix: "kvtest"
  value_size: 4096
  seed: 20260527
  count: 10000
  value_generator: "[value生成算法待实现确认]"
  digest_algorithm: "[摘要算法待实现确认]"

limits:
  batch_store_max: 110
  batch_retrieve_max: 110
  delete_max: 254
  exist_max: 256

consistency:
  enabled: true
  compare_value: true
  stop_on_first_error: false

bench:
  duration_sec: 300
  warmup_sec: 30
  io_size: 4096
  concurrency: 64
  read_ratio: 70
  write_ratio: 30
  batch_size: 64

output:
  path: "./results/kv-test"
  formats: ["text", "json", "csv"]
  realtime_file_max_bytes: 104857600
```

字段约束：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `connection.target` | 字符串 | 是 | ASU 服务地址或设备标识，具体格式为 `[ASU建链配置字段]`。 |
| `connection.protocol` | 枚举 | 是 | `UB`、`ROCE` 或 `TCP`。 |
| `connection.timeout_ms` | 无符号 64 位整数 | 是 | 默认请求超时时间。 |
| `connection.retry` | 无符号 32 位整数 | 是 | 建链或请求失败后的重试次数。 |
| `kv.key_prefix` | 字符串 | 是 | 使用 `--count` 生成 key 时的 key 前缀。 |
| `kv.value_size` | 无符号 64 位整数 | 是 | 默认 value 大小。 |
| `kv.seed` | 无符号 64 位整数 | 是 | 默认随机数种子。 |
| `limits.batch_store_max` | 无符号 32 位整数 | 是 | BatchStore 最大条目数，规格值为 `110`。 |
| `limits.batch_retrieve_max` | 无符号 32 位整数 | 是 | BatchRetrieve 最大条目数，规格值为 `110`。 |
| `limits.delete_max` | 无符号 32 位整数 | 是 | Delete 单次最大条目数，规格值为 `254`。 |
| `limits.exist_max` | 无符号 32 位整数 | 是 | Exist 单次最大条目数，规格值为 `256`。 |
| `output.realtime_file_max_bytes` | 无符号 64 位整数 | 是 | 实时 CSV 文件大小上限，超过后滚动到下一个文件。 |

## 输出

### 结果目录

```text
results/kv-test/
  run-[timestamp]/
    config.yaml
    summary.txt
    summary.json
    bench-realtime-0.csv
    bench-realtime-1.csv
    latency.csv
    consistency_errors.jsonl
    kv-test.log
```

实时数据文件命名规则：

- 第一个实时文件名为 `bench-realtime-0.csv`。
- 单个文件大小达到 `output.realtime_file_max_bytes` 后，新建 `bench-realtime-1.csv`。
- 后缀按 `-0`、`-1`、`-2` 递增。

### 汇总 JSON

```json
{
  "tool": "kv-test",
  "command": "bench",
  "status": "success",
  "exit_code": 0,
  "configpath": "./config/asu_kv_test.yaml",
  "start_time": "2026-05-28T10:00:00+08:00",
  "end_time": "2026-05-28T10:05:00+08:00",
  "connection": {
    "target": "[ASU服务地址或设备标识]",
    "protocol": "TCP",
    "status": "success"
  },
  "request": {
    "op": "batch-retrieve",
    "key_count": 10000,
    "value_size": 4096,
    "batch_size": 64,
    "concurrency": 64,
    "duration_sec": 300
  },
  "metrics": {
    "bandwidth": {
      "avg": {"value": 0.0, "unit": "MB/s"},
      "realtime_file_pattern": "realtime-*.csv"
    },
    "iops": {
      "avg": {"value": 0.0, "unit": "1/s"},
      "avg_batch": {"value": 0.0, "unit": "1/s"}
    },
    "latency": {
      "avg": {"value": 0.0, "unit": "us"},
      "min": {"value": 0.0, "unit": "us"},
      "max": {"value": 0.0, "unit": "us"},
      "p99_9": {"value": 0.0, "unit": "us"},
      "p99_99": {"value": 0.0, "unit": "us"},
      "p99_999": {"value": 0.0, "unit": "us"}
    }
  },
  "consistency": {
    "enabled": true,
    "checked": 10000,
    "passed": 10000,
    "failed": 0,
    "pass_rate": 1.0
  },
  "error": null
}
```

字段说明：

| 字段 | 可为空 | 说明 |
| --- | --- | --- |
| `status` | 否 | `success`、`partial_failed` 或 `failed`。 |
| `exit_code` | 否 | 进程退出码。 |
| `metrics` | 是 | 非性能命令可为 `null`。 |
| `consistency` | 是 | 未启用一致性检查时可为 `null`。 |
| `error` | 是 | 成功时为 `null`，失败时使用错误结构。 |

### 错误 JSON

```json
{
  "tool": "kv-test",
  "command": "retrieve",
  "status": "failed",
  "exit_code": 4,
  "error": {
    "code": "CONSISTENCY_FAILED",
    "message": "value 不一致",
    "asu_status_code": "OK",
    "retryable": false,
    "request_id": "[请求编号]",
    "key": "key1"
  }
}
```

### 实时 CSV

```csv
timestamp_sec,op,bandwidth_value,bandwidth_unit,iops_value,iops_unit,avg_latency_value,avg_latency_unit,error_count
1,batch-retrieve,[待实测],MB/s,[待实测],1/s,[待实测],us,0
2,batch-retrieve,[待实测],MB/s,[待实测],1/s,[待实测],us,0
```

### 时延 CSV

```csv
op,avg_value,avg_unit,min_value,min_unit,max_value,max_unit,p99_9_value,p99_9_unit,p99_99_value,p99_99_unit,p99_999_value,p99_999_unit
batch-retrieve,[待实测],us,[待实测],us,[待实测],us,[待实测],us,[待实测],us,[待实测],us
```

## 指标规则

### 单位格式

| 指标 | 单位 | 格式规则 |
| --- | --- | --- |
| IOPS | `1/s` | 保留 2 位小数。 |
| 带宽 | `B/s`、`KB/s`、`MB/s`、`GB/s` | 保留 2 位小数，选择数值小于 `1000` 的最大单位。 |
| 时延 | `s`、`ms`、`us`、`ns` | 保留 2 位小数，选择数值小于 `1000` 的最大单位。 |

### 批量指标

批量维度只统计 Batch 命令本身：

- Batch IOPS：每秒完成的 BatchStore 或 BatchRetrieve 命令数量。
- Batch 带宽：已完成 Batch 命令包含的总数据量除以统计窗口时长。
- Batch 时延：单个 Batch 命令从下发到完成的耗时。

不统计“批内单条命令维度带宽”。如果需要观察批内条目数，可通过 `batch_size` 和完成的 Batch 命令数量推导。

## 一致性规则

| 操作 | 检查规则 |
| --- | --- |
| `Store` | Store 成功后，Retrieve 同一 key，返回 value 必须与期望 value 一致。 |
| `Retrieve` | 返回 value 必须与按 `seed`、`value_size`、`key` 生成的期望 value 一致。 |
| `Delete` | Delete 成功后，Exist 必须返回不存在，Retrieve 不得返回旧 value。 |
| `Exist` | Store 后必须返回存在，Delete 后必须返回不存在。 |
| `BatchStore` | Batch 内每个 entry 必须满足 Store 检查规则。 |
| `BatchRetrieve` | Batch 内每个 entry 必须满足 Retrieve 检查规则。 |
| `power-cycle verify` | `prepare` 阶段成功写入的 key，重新上电后必须可 Retrieve，且 value 完全一致。 |

Delete 不存在 key 的规则：

- 底层接口返回失败状态码到 AsuClient。
- `kv-test` 将该场景记录为 Delete 请求失败。
- 如果命令启用 `--check`，该场景导致一致性检查失败。

一致性失败明细使用 JSONL：

```json
{"key":"key1","op":"retrieve","expected_hash":"[期望value摘要]","actual_hash":"[实际value摘要]","request_id":"[请求编号]","error_code":"CONSISTENCY_FAILED"}
```

`value_generator` 和 `digest_algorithm` 的具体算法为 `[value生成算法和摘要算法待确认]`。实现前不得把随机值、哈希算法或摘要长度写死到调用方逻辑中。

## 命令

### connect

功能：读取配置文件并建立 ASU 连接。

```bash
kv-test connect --configpath ./config/asu_kv_test.yaml
```

成功条件：

- 配置文件解析成功。
- ASU 客户端初始化成功。
- 建链成功。

失败条件：

- 配置文件缺失或字段非法，返回 `INVALID_ARGUMENT`。
- ASU 客户端未初始化，返回 `NOT_INITIALIZED`。
- 建链超时，返回 `TIMEOUT`。
- 建链失败，返回 `CONNECTION_ERROR`。

### store

功能：写入一个或多个 KV。

```bash
kv-test store --key key1 --configpath ./config/asu_kv_test.yaml --check
kv-test store --keys key1,key2,key3 --configpath ./config/asu_kv_test.yaml
kv-test store --count 10000 --seed 20260527 --configpath ./config/asu_kv_test.yaml
```

输出：

- 总成功数、失败数、耗时。
- 每个失败的 Store 状态。
- 启用 `--check` 时输出一致性结果。

功能说明：
- 使用 store 功能最终实现为逐个 key 调用底层 Store 接口；
- 使用 `--key`、`--keys` 指定时，随机生成指定 key 对应的 value 张量；
- 使用 `--count` 随机时，按顺序生成 `key1`、`key2` 等 key，并随机生成对应的 value 张量。

### retrieve

功能：读取一个或多个 KV。

```bash
kv-test retrieve --key key1 --configpath ./config/asu_kv_test.yaml --check
kv-test retrieve --keys key1,key2,key3 --configpath ./config/asu_kv_test.yaml --check
```

输出：

- 总成功数、失败数、耗时。
- 每个失败的 Retrieve 状态。
- 启用 `--check` 时输出 value 比对结果。

功能说明：
- 使用 retrieve 功能最终实现为逐个 key 调用底层 Load 接口；

### delete

功能：删除一个或多个 KV。

```bash
kv-test delete --key key1 --configpath ./config/asu_kv_test.yaml --check
kv-test delete --keys key1,key2,key3 --configpath ./config/asu_kv_test.yaml --check
```

约束：无。

功能说明：
客户端接收的 keys 会首先被 Router 分散到各个 Transport，每个 Transport 在切分 IO 的阶段用配置项
 `limits.delete_max` 进行切分。

### exist

功能：查询一个或多个 key 是否存在。

```bash
kv-test exist --key key1 --configpath ./config/asu_kv_test.yaml
kv-test exist --keys key1,key2,key3 --configpath ./config/asu_kv_test.yaml
```

约束：无。

功能说明：
客户端接收的 keys 会首先被 Router 分散到各个 Transport，每个 Transport 在切分 IO 的阶段用配置项
 `limits.exist_max` 进行切分。

### batch-store

功能：下发 BatchStore。

```bash
kv-test batch-store --count 10000 --seed 20260527 --batch-size 64 --configpath ./config/asu_kv_test.yaml --check
```

约束：

- 超过限制时返回参数错误，不自动提升规格上限。

功能说明：
客户端接收的 keys 会首先被 Router 分散到各个 Transport，每个 Transport 在切分 IO 的阶段用配置项 `limits.batch_store_max` 进行切分。

### batch-retrieve

功能：下发 BatchRetrieve。

```bash
kv-test batch-retrieve --count 10000 --seed 20260527 --batch-size 64 --configpath ./config/asu_kv_test.yaml --check
```

约束：

- `--batch-size` 不得超过 `limits.batch_retrieve_max`，规格值为 `110`。
- 超过限制时返回参数错误，不自动提升规格上限。

### power-cycle prepare

功能：掉电一致性测试的写入阶段。

```bash
kv-test power-cycle prepare \
  --configpath ./config/asu_kv_test.yaml \
  --seed 20260527 \
  --count 100000 \
  --value-size 4096 \
  --op batch-store \
  --batch-size 64
```

输出：

- 写入成功 key 数。
- 写入失败 key 数。
- 测试元数据：`seed`、`count`、`value_size`、`key_prefix`、`op`、`batch_size`。
- `prepare` 结果文件路径。

### power-cycle verify

功能：掉电后重新上电的数据一致性校验阶段。

```bash
kv-test power-cycle verify \
  --configpath ./config/asu_kv_test.yaml \
  --seed 20260527 \
  --count 100000 \
  --value-size 4096 \
  --op batch-retrieve \
  --batch-size 64 \
  --check
```

执行顺序：

1. 测试人员执行 `power-cycle prepare`。
2. 测试人员或外部系统完成 ASU 下电和上电，操作步骤为 `[ASU下电上电操作步骤]`。
3. 测试人员执行 `power-cycle verify`。
4. 工具用同一 `seed`、`count`、`value_size` 和 `key_prefix` 生成同一批 key/value。
5. 工具执行 Retrieve 或 BatchRetrieve。
6. 工具输出一致性统计。

通过条件：

- `prepare` 阶段成功写入的 key 全部 Retrieve 成功。
- 返回 value 与期望 value 完全一致。
- 一致性通过率为 `100%`。

### bench

功能：执行 KV 性能测试。

底层调用 BatchLoad：
```bash
kv-test bench \
  --configpath ./config/asu_kv_test.yaml \
  --op retrieve \
  --io-size 4096 \
  --concurrency 64 \
  --duration 300 \
  --read-ratio 100 \
  --write-ratio 0
```

底层调用 BatchStore：
```bash
kv-test bench \
  --configpath ./config/asu_kv_test.yaml \
  --op store \
  --io-size 4096 \
  --concurrency 64 \
  --duration 300 \
  --read-ratio 0 \
  --write-ratio 100
```

底层调用 BatchStore、BatchLoad：
```bash
kv-test bench \
  --configpath ./config/asu_kv_test.yaml \
  --op mix \
  --io-size 4096 \
  --concurrency 128 \
  --duration 600 \
  --read-ratio 70 \
  --write-ratio 30
```

统计指标：

- 平均带宽。
- 平均 IOPS。
- 平均时延。
- 最大时延。
- 最小时延。
- 实时每秒带宽。
- 实时每秒 IOPS。
- IO 一致性统计。
- P99.9 时延。
- P99.99 时延。
- P99.999 时延。

## 示例

### 正常示例

```bash
kv-test batch-store --count 10000 --seed 20260527 --batch-size 64 --configpath ./config/asu_kv_test.yaml --check
kv-test batch-retrieve --count 10000 --seed 20260527 --batch-size 64 --configpath ./config/asu_kv_test.yaml --check
```

预期结果：

- BatchStore 全部成功。
- BatchRetrieve 全部成功。
- 所有 value 一致性检查通过。
- 输出 `summary.json`、`realtime-0.csv`、`latency.csv` 和日志文件。

### 边界示例

```bash
kv-test batch-store --count 110 --seed 20260527 --batch-size 110 --configpath ./config/asu_kv_test.yaml
kv-test delete --count 254 --seed 20260527 --configpath ./config/asu_kv_test.yaml
kv-test exist --count 256 --seed 20260527 --configpath ./config/asu_kv_test.yaml
```

预期结果：

- `batch-store` 使用最大 BatchStore entry 数 `110`。
- `delete` 使用最大 Delete entry 数 `254`。
- `exist` 使用最大 Exist entry 数 `256`。

### 错误示例

```bash
kv-test batch-retrieve --count 111 --seed 20260527 --batch-size 111 --configpath ./config/asu_kv_test.yaml
```

预期结果：

```json
{
  "tool": "kv-test",
  "command": "batch-retrieve",
  "status": "failed",
  "exit_code": 1,
  "error": {
    "code": "INVALID_ARGUMENT",
    "message": "batch_size exceeds limits.batch_retrieve_max",
    "asu_status_code": null,
    "retryable": false,
    "request_id": null,
    "key": null
  }
}
```

## 错误处理

### 退出码

| 退出码 | 含义 | 可重试 |
| --- | --- | --- |
| `0` | 命令成功，启用的检查全部通过。 | 否 |
| `1` | 参数错误或配置文件错误。 | 否 |
| `2` | 建链失败。 | 是 |
| `3` | KV 请求发送失败或响应超时。 | 是 |
| `4` | 一致性检查失败。 | 否 |
| `5` | 性能测试执行失败。 | 按错误类型判断 |
| `6` | 部分请求失败，且失败策略允许继续执行。 | 按失败 entry 判断 |
| `>=100` | ASU 底层状态码映射。 | 按状态码判断 |

### ASU 状态码映射

`kv-test` 使用 `UC::ASU::StatusCode` 作为底层错误来源，定义位置为 `ucm/transport/kv/asu/trans/include/asu_transport/types.h`。

| StatusCode | 值 | 建议错误码 | 可重试 |
| --- | --- | --- | --- |
| `OK` | `0` | `OK` | 否 |
| `INVALID_ARGUMENT` | `1` | `INVALID_ARGUMENT` | 否 |
| `NOT_INITIALIZED` | `2` | `NOT_INITIALIZED` | 否 |
| `TIMEOUT` | `3` | `TIMEOUT` | 是 |
| `NOT_FOUND` | `4` | `NOT_FOUND` | 否 |
| `PARTIAL_FAILED` | `5` | `PARTIAL_FAILED` | 按失败 entry 判断 |
| `CONNECTION_ERROR` | `6` | `CONNECTION_ERROR` | 是 |
| `IO_ERROR` | `7` | `IO_ERROR` | 是 |
| `BUFFER_NOT_REGISTERED` | `8` | `BUFFER_NOT_REGISTERED` | 否 |
| `BUFFER_NOT_SUPPORTED` | `9` | `BUFFER_NOT_SUPPORTED` | 否 |
| `TASK_NOT_FOUND` | `10` | `TASK_NOT_FOUND` | 否 |
| `RESOURCE_BUSY` | `11` | `RESOURCE_BUSY` | 是 |
| `UNSUPPORTED` | `12` | `UNSUPPORTED` | 否 |
| `IN_PROGRESS` | `13` | `IN_PROGRESS` | 是 |
| `INTERNAL_ERROR` | `14` | `INTERNAL_ERROR` | 是 |
| `CANCELED` | `15` | `CANCELED` | 按取消原因判断 |

### 幂等性

| 命令 | 幂等性 |
| --- | --- |
| `connect` | 幂等。重复执行应返回连接成功或已连接。 |
| `store` | 对同一 key 和相同 value 重复执行后，最终 value 应一致。 |
| `retrieve` | 幂等。 |
| `delete` | 对已存在 key 首次执行成功；对不存在 key 执行时，底层返回失败状态码。 |
| `exist` | 幂等。 |
| `batch-store` | 批内 entry 按 Store 规则处理。 |
| `batch-retrieve` | 幂等。 |
| `power-cycle prepare` | 不保证幂等；重复执行会重新写入同一批 key。 |
| `power-cycle verify` | 幂等。 |
| `bench` | 不保证幂等；写压测会改变目标 key 的 value。 |

### 超时行为

- 单请求超时后，工具将该请求标记为 `TIMEOUT`。
- 如果配置允许重试，工具最多重试 `connection.retry` 次。
- 重试仍失败时，命令返回非零退出码。
- 批量请求返回 `PARTIAL_FAILED` 时，工具必须记录每个失败 entry 的状态。

## 工具关系

推荐调用链：

```text
connect -> store -> retrieve --check -> exist -> delete --check
```

批量功能验证：

```text
connect -> batch-store --check -> batch-retrieve --check
```

掉电一致性验证：

```text
connect -> power-cycle prepare -> [ASU下电上电操作] -> connect -> power-cycle verify --check
```

性能测试：

```text
connect -> bench --op store
connect -> bench --op retrieve
connect -> bench --op mix
connect -> bench --op batch-store
connect -> bench --op batch-retrieve
```

替代关系：

- 需要单条请求语义时使用 `store`、`retrieve`、`delete`、`exist`。
- 需要批量请求语义时使用 `batch-store`、`batch-retrieve`。
- 需要掉电前后数据一致性时使用 `power-cycle prepare` 和 `power-cycle verify`。
- 需要 IOPS、带宽、时延和 QOS 指标时使用 `bench`。

## Local/offline validation additions

- `asu.client.mode=local` is a kv-test-only mode. It uses a file-system backed ASU transport so Store/Retrieve/Delete/Exist can be validated without ViewServer, network, Hcomm, or ASU hardware.
- `local_store.path=<path>` selects the persistent local KV directory used by local mode. Files remain across separate `kv-test` commands.
- `view.config_path=<path>` selects the ASU view config file. Initial view load and later refresh read this file; view changes are made by editing the file externally.
- Example config: `ucm/transport/kv/kv-test/asu_kv_test.conf`.
- Example view config: `ucm/transport/kv/kv-test/asu_view.conf`.
