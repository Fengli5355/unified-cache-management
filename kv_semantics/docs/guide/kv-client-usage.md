# KVClient 使用指南

KVClient 是面向 ASU KV 服务的异步 C++ 客户端。它负责把 key 路由到对应 ASU，管理传输连接、异步任务和业务内存注册，并把多个 ASU 的结果重新合并为调用方提交时的顺序。

仓库中有两种主要调用方式：

- `kv-test`：命令行验证工具，适合验证配置、连通性、基本读写和性能。
- `ASUStore`：UCM Store 适配层，供 vLLM/vLLM-Ascend 推理流程使用。

直接集成 C++ 库时，调用方也可以通过 `kv::CreateKvClient()` 使用同一套能力。API 的完整约束见 [KVClient API 与行为约定](kv-client-api.md)。

## 1. 组件和产物

| 产物 | 作用 |
| --- | --- |
| `libkv_client.so` | 对外提供 KVClient，负责路由、任务聚合、视图刷新和内存注册。 |
| `libkv_transport.so` | 负责每个 ASU 的连接、协议封装、发送和完成处理。 |
| `libasustore.so` | 把 UCM Store 接口转换为 KVClient 调用。 |
| `kv-test` | 基于 KVClient 的命令行测试程序。 |

Provider 决定最底层的数据通路：

| Provider | 使用场景 | 构建开关 |
| --- | --- | --- |
| `FAKE` | 本地功能验证，不依赖真实 ASU。 | `BUILD_KV_CLIENT_PROVIDER_FAKE=ON` |
| `AIV` | 使用 AIV/UMC 数据通路访问真实 ASU。 | `BUILD_KV_CLIENT_PROVIDER_AIV=ON` |
| `AICPU` | 使用 HCOMM AICPU 数据通路访问真实 ASU。 | `BUILD_KV_CLIENT_PROVIDER_AICPU=ON` |

运行时配置的 provider 必须已经编译进 `libkv_transport.so`。

## 2. 用 kv-test 跑通基本功能

以下流程以 AICPU provider 的 UBG 数据通路为准，要求已安装匹配的 CANN/HCOMM/HIXL 依赖，并已启动匹配的 ASU staged server（`--conn-mode rm`）。

### 2.1 构建

在仓库根目录执行：

```bash
BUILD_KV_CLIENT_PROVIDER_AICPU=ON bash kv_semantics/kv_test/build.sh
```

脚本构建 AICPU provider，产物默认位于 `build-kv-test`，可通过 `BUILD_DIR` 覆盖。

### 2.2 准备 AICPU 配置

使用 `kv_semantics/kv_test/kv_test_aicpu.conf` 及其对应的 `kv_semantics/kv_test/kv_view_aicpu.conf`。运行前修改：

- `transport.aicpu_local_eid.<logical_device_id>`：各 ACL 逻辑设备的本地 UB EID。
- 两个文件中的 `asu_info.<id>`：远端 ASU EID 和 RaSocket OOB 监听端口，保持一致。
- `transport.asu_ids` 与视图中的 `asuIds`：保持一致；`transport.kv_ns_id` 与服务端 namespace 对齐。

该示例使用 `transport.aicpu_hcomm_protocol=ubg`，要求 EID 地址，不执行 IP 到 EID 的自动发现。若使用 UBC_CTP，按配置文件注释修改协议及本地、远端地址，并让服务端使用匹配的数据通路。配置中的相对路径按进程工作目录解析，以下命令均在仓库根目录运行。

当前 EID 由用户显式写入配置文件（本地设备 EID 和远端 ASU EID）；这属于当前实现的配置方式。未来需要改为自动发现 EID，不再将 EID 写入配置文件；该自动发现能力尚未实现。

### 2.3 运行基本指令

```bash
source kv_semantics/kv_test/set_kvtest_env.sh

# 检查配置并建立 Client
kv-test config check
kv-test connect

# 写入、查询、读取和删除同一批数据
kv-test store --count 4 --check
kv-test exist --count 4
kv-test retrieve --count 4 --check
kv-test delete --count 4
kv-test exist --count 4
```

`kv.seed`、`kv.key_prefix` 和 `kv.value_size` 决定测试数据。store 和 retrieve 使用相同配置时会生成相同内容，因此 `--check` 可以验证数据一致性。

一个简短性能测试示例：

```bash
kv-test bench mix \
  --duration 10 \
  --concurrency 1 \
  --batch-size 16 \
  --read-ratio 50 \
  --write-ratio 50 \
  --progress
```

`--concurrency` 不表示 IO 并发数；原异步设计中表示 Wait 线程数，当前同步 bench 不创建该线程池，逐批提交并等待完成，实际在途批次为 1。

完整命令和输出格式见 [kv-test command line tool](kv-test.md)。

## 3. 通过 ASUStore 跑通推理

这一方式适合验证实际调用链：

```text
vLLM / vLLM-Ascend
  -> UCMConnector
  -> UcmPipelineStore
  -> ASUStore
  -> KVClient
  -> ASU
```

以下示例以 vLLM 的 OpenAI 兼容服务为例。运行前需要满足：

- vLLM 或 vLLM-Ascend 与当前 UCM 版本匹配。
- UCM 已启用 ASUStore 和目标 provider。
- 真实 ASU 服务已经启动，地址、端口和 namespace 已分配。
- 模型 KV 布局受到当前 UCM/ASU 组合支持。

### 3.1 构建并安装 UCM

AICPU 示例（先准备匹配的 CANN/HCOMM 环境）：

```bash
export PLATFORM=ascend
export BUILD_UCM_ASU=1
export BUILD_KV_CLIENT_PROVIDER_AICPU=1
pip install -v -e . --no-build-isolation
```

### 3.2 准备 ASUStore 配置

复制 `examples/ucm_config_asu_aicpu.yaml`，至少替换以下内容：

```yaml
ucm_connectors:
  - ucm_connector_name: "UcmPipelineStore"
    ucm_connector_config:
      store_pipeline: "ASU"
      asu_mode: "client"
      asu_client_id: "inference-worker-0"
      asu_ids: [1]
      asu_ips: ["eid:<REMOTE_ASU_EID>"]
      asu_ports: [19001]
      asu_trans_provider_backend: "aicpu"
      asu_aicpu_hcomm_protocol: "ubg"
      asu_aicpu_local_addrs: ["eid:<LOCAL_DEVICE_0_EID>"]
      asu_aicpu_send_timeout_ms: 30000
      asu_shared_provider: 0
      kv_ns_ids: [1]
      asu_wait_timeout_ms: 5000
      asu_query_timeout_ms: 500

enable_event_sync: true
use_layerwise: false
persist_token_threshold: 0
```

这里显式选用 UBG，与上面的 kv-test 示例一致；仓库 YAML 示例默认使用 UBC_CTP，复制后需按实际服务端协议调整。`asu_aicpu_local_addrs` 的下标是 ACL 逻辑设备 ID，多设备进程必须逐项配置；不要在 YAML 中固定 `device_id`，各 TP worker 使用当前 ACL 逻辑设备。

当前 EID 由用户显式写入配置文件（本地设备 EID 和远端 ASU EID）；这属于当前实现的配置方式。未来需要改为自动发现 EID，不再将 EID 写入配置文件；该自动发现能力尚未实现。

`asu_ids`、`asu_ips` 和 `asu_ports` 按下标一一对应。普通模型使用一个 `kv_ns_ids`；FAWA 模型需要分别配置 FA 和 WA namespace。详细字段见 [KVClient 与 ASUStore 配置说明](kv-client-configuration.md)。

### 3.3 启动推理服务

```bash
export ENABLE_UCM_PATCH=1

vllm serve "/absolute/path/to/model" \
  --max-model-len 32768 \
  --max-num-batched-tokens 8192 \
  --served-model-name asu-model \
  --gpu-memory-utilization 0.85 \
  --data-parallel-size 1 \
  --tensor-parallel-size 1 \
  --port 7800 \
  --block-size 128 \
  --compilation-config '{"cudagraph_mode": "FULL_DECODE_ONLY"}' \
  --async-scheduling \
  --additional-config '{
    "ascend_compilation_config": {
      "enable_npugraph_ex": true,
      "enable_static_kernel": false
    },
    "enable_cpu_binding": true
  }' \
  --kv-transfer-config '{
    "kv_connector": "UCMConnector",
    "kv_connector_module_path": "ucm.integration.vllm.ucm_connector",
    "kv_role": "kv_both",
    "kv_connector_extra_config": {
      "UCM_CONFIG_FILE": "examples/ucm_config_asu_aicpu.yaml"
    }
  }'
```

看到 vLLM 完成 application startup，并且日志中出现 `AsuStore` 和有效配置，说明 Store 已成功初始化。随后发送一次请求：

```bash
curl http://127.0.0.1:7800/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "asu-model",
    "prompt": "Explain why KV cache reuse reduces prefill cost.",
    "max_tokens": 32,
    "temperature": 0
  }'
```

用相同 prompt 再请求一次，并检查服务日志和 KV 指标。第二次请求应出现外部 KV 命中；是否减少 TTFT 还取决于模型、数据通路和缓存写入是否已完成。

## 4. 直接调用 C++ KVClient

最小调用流程如下。示例省略了平台相关的设备内存申请，实际业务必须把真实地址和长度传给 `RegisterRegions()`。

```cpp
#include "kv_client.h"

auto client = kv::CreateKvClient();

kv::KvClientConfig config;
auto status = kv::KvClientConfig::LoadFromFile("/absolute/path/to/kv_test_aicpu.conf", config);
if (!status.ok()) { return; }

status = client->Init(config);
if (!status.ok()) { return; }

std::vector<kv::MemoryRegion> regions = {
    {kv::MemoryType::DEVICE, deviceAddress, bufferBytes, deviceId, -1},
};
std::vector<kv::RegisteredMemory> registered;
status = client->RegisterRegions(regions, registered);
if (!status.ok()) { return; }

kv::KVBuffer entry;
entry.key = key;
entry.buffer.region = registered[0].region;
entry.buffer.handle = registered[0].handle;
entry.offset = 0;

kv::TaskId taskId = kv::kInvalidTaskId;
status = client->StoreAsync({entry}, taskId);
if (status.ok()) {
    kv::TaskResult result;
    status = client->Wait(taskId, 5000, result);
}

client->UnregisterRegions({registered[0].handle});
client->Shutdown();
```

业务 buffer 和注册 handle 必须保持有效，直到对应任务完成。内存约束见 [KVClient 内存与 Buffer 约定](kv-client-memory.md)。

## 5. 推荐验证顺序

1. 用 FAKE provider 跑通 kv-test 的 store、retrieve 和 delete。
2. 切换真实 provider，用 `kv-test connect` 和小批量 store/retrieve 验证连接和协议。
3. 用 ASUStore 启动单 worker、单 ASU 推理。
4. 确认重复请求命中后，再扩展到多 ASU、TP/DP 和性能测试。

这样可以把配置、传输和推理集成问题分层定位，避免一开始就在完整推理环境中排查所有组件。
