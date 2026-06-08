# Sub-Batch Trace 使用指南

## 这是什么

一个诊断工具，把每次 `StoreAsync` / `LoadAsync` / `DeleteAsync` / `QueryAsync` 操作的
sub-batch 拆分、分配、发送、完成过程打印成一张表格。

设环境变量 `ASU_TRACE=1` 即可开启，**调用方代码不用改**。

## 文件变更

| 文件 | 改动 |
|---|---|
| `trans/src/sub_batch_trace.h` | **新增** — 快照结构 + 函数声明 |
| `trans/src/sub_batch_trace.cpp` | **新增** — 捕获、格式化、枚举转字符串 |
| `trans/src/asu_transport_impl.h` | **修改** — 加了 `traceEnabled_` 成员 |
| `trans/src/asu_transport_impl.cpp` | **修改** — Init 读环境变量，ProcessTask 和 PollTaskCompletions 各加一个 `if (traceEnabled_)` 埋点 |

---

## 怎么用

### 1. 设置环境变量

```bash
export ASU_TRACE=1
```

或者运行时前面加：

```bash
ASU_TRACE=1 ./your_program
```

不设或设成其他值则关闭（`ASU_TRACE=0` 也是关闭）。Init 时会打一条 INFO 日志确认是否开启：

```
AsuTransportImpl::Init sub-batch trace enabled (ASU_TRACE=1)
```

### 2. 正常运行你的程序

你的业务代码不用改，正常调 API 就行：

```cpp
auto transport = CreateAsuTransport();
transport->Init(config);

auto entries = MakeKVEntries(10);
TaskId taskId;
transport->StoreAsync(entries, taskId);

TaskResult result;
transport->Wait(taskId, 5000, result);

transport->Shutdown();
```

### 3. stdout 自动输出两张表

**表 1：ProcessTask 发送后**（channel/slot/send_flag 全在，信息最完整）

```
ASU transport trace: task_id=42 op=BATCH_STORE total_entries=10 sub_batches=3 task_state=INFLIGHT task_status=OK

idx  key_range   size  cid   channel         send_slot  flag_slot  state       final_status
0    [0..3]      4     17    group=0,ch=1    3          8          PENDING     OK
1    [4..7]      4     18    group=0,ch=2    4          9          PENDING     OK
2    [8..9]      2     19    group=1,ch=0    5          10         PENDING     OK
```

**表 2：任务完成后**（channel/slot 已释放显示 `--`，但 state/status 是最终值）

```
ASU transport trace: task_id=42 op=BATCH_STORE total_entries=10 sub_batches=3 task_state=COMPLETED task_status=OK

idx  key_range   size  cid   channel         send_slot  flag_slot  state       final_status
0    [0..3]      4     17    --              --         --         COMPLETED   OK
1    [4..7]      4     18    --              --         --         COMPLETED   OK
2    [8..9]      2     19    --              --         --         COMPLETED   OK
```

---

## ProcessTask 里的打印位置

Trace 的调用链是：

```text
QueryAsync / LoadAsync / StoreAsync / DeleteAsync
        ↓
SubmitAsync
        ↓
executeQueue_
        ↓
WorkerLoop
        ↓
ProcessTask
        ↓
如果 ASU_TRACE=1，就打印 trace
```

`ProcessTask` 只在 `WorkerLoop` 消费执行队列时调用：

```cpp
void AsuTransportImpl::WorkerLoop()
{
    executeQueue_.ConsumerLoop(stop_, [this](TransportTaskContextPtr ctx) {
        if (!ctx) { return; }
        ProcessTask(ctx);
    });
}
```

所以所有通过 ASU async API 提交进队列的任务都会走 `ProcessTask`，包括：

- `QueryAsync`
- `LoadAsync`
- `StoreAsync`
- `DeleteAsync`

第一张 trace 表不是等 `ProcessTask` 整个函数执行完才打印，而是在 `ProcessTask` 函数内部，执行到发送流程之后立刻打印。位置大致是：

```text
1. task 从 PENDING 改成 INFLIGHT
2. SubmitTaskRequests 拆 sub-batch
3. AssignSubBatchConnections 分配 channel
4. BuildSubBatchSendBuffers 分配 send/flag buffer
5. SendSubBatchBuffers 发送
6. 把 subBatchContexts 保存到 ctx
7. 打印 trace
```

代码上是通过语句顺序控制的：`trace::PrintTraceTable(...)` 写在 `SendSubBatchBuffers(...)` 和 `ctx->subBatchContexts = ...` 之后，所以只有前面的拆分、分配、构建 buffer、发送都执行到这里，才会打印表 1。

```cpp
auto sendStatus = SendSubBatchBuffers(subBatchContexts, ioBatches, subBatchIndexes);

if (!subBatchContexts.empty()) {
    ctx->subBatchContexts = std::move(subBatchContexts);
}
ctx->finalStatus = finalStatus;

if (traceEnabled_) {
    trace::PrintTraceTable(...);
}
```

也就是说，表 1 是 **发送完成之后、资源释放之前** 的快照，所以能看到 `channel`、`cid`、`send_slot`、`flag_slot`。

表 2 不是 `ProcessTask` 打印的，而是在完成轮询线程里打印：

```text
CompletionLoop
        ↓
PollTaskCompletions
        ↓
ctx->TryFinalizeFromSubBatches()
        ↓
if (ctx->Done())
        ↓
if (traceEnabled_) 打印表 2
```

所以完整理解是：

- 表 1：`ProcessTask()` 内部，`SendSubBatchBuffers()` 之后打印
- 表 2：`PollTaskCompletions()` 内部，`ctx->Done()` 之后打印

这些情况不会打印 ProcessTask 里的 trace：

- `ASU_TRACE` 没设成 `1`
- task 不是 `PENDING`，`ProcessTask` 一开始就 skip
- task 在发送后发现已经 `CANCELED`，代码会释放资源并 return
- `subBatchContexts` 为空时会打印表头，但不会有 sub-batch 明细行

如果想把输出和某次 `ProcessTask` 对上，看 trace 表头里的 `task_id` 和 `op`：

```text
ASU transport trace: task_id=42 op=BATCH_STORE ...
```

然后在日志里找相同的任务 ID：

```text
AsuTransportImpl::ProcessTask start task_id=42
AsuTransportImpl::ProcessTask send done task_id=42
```

---

## 出错时的效果

```
ASU transport trace: task_id=43 op=QUERY total_keys=9 sub_batches=3 task_state=COMPLETED task_status=PARTIAL_FAILED: one or more sub-batches failed

idx  key_range   size  cid   channel         send_slot  flag_slot  state       final_status
0    [0..2]      3     20    --              --         --         COMPLETED   OK
1    [3..5]      3     21    --              --         --         COMPLETED   CONNECTION_ERROR: fake send failure
2    [6..8]      3     --    --              --         --         PENDING     INTERNAL_ERROR: skipped after submit failure
```

---

## 两张表的区别

| | 表 1（发送后） | 表 2（完成后） |
|---|---|---|
| **埋点位置** | ProcessTask: send 之后、资源释放前 | PollTaskCompletions: Done 之后 |
| **channel** | ✅ 有值 | `--`（已释放） |
| **send_slot / flag_slot** | ✅ 有值 | `--`（已释放） |
| **state** | PENDING（还没回包） | COMPLETED |
| **final_status** | OK（初始值） | 真实错误码 |
| **用途** | 看拆分/分配/发送是否正确 | 看完成/错误是否符合预期 |

---

## 输出怎么读

### 表头

```
ASU transport trace: task_id=42 op=BATCH_STORE total_entries=10 sub_batches=3 task_state=COMPLETED task_status=OK
```

- `task_id` — 任务 ID
- `op` — 操作类型
- `total_entries` / `total_keys` — 原始 batch 总大小（entry 类操作用 entries，key 类用 keys）
- `sub_batches` — 拆成了几个 sub-batch
- `task_state` — PENDING / INFLIGHT / COMPLETED / CANCELED
- `task_status` — 最终状态

### 各列含义

| 列 | 含义 | 出问题时看什么 |
|---|---|---|
| `idx` | sub-batch 序号 | — |
| `key_range` | 原始 batch 中的索引范围 | 是否连续、无重叠、无遗漏 |
| `size` | entry/key 数量 | 是否符合 ioNum 配置 |
| `cid` | Command ID（单调递增） | 是否连续、和 sub-batch 一一对应 |
| `channel` | `group=连接组,ch=通道号` | 是否全打同一个、failed 的是否被选 |
| `send_slot` | 发送 buffer slot | 是否分配了、完成后是否释放 |
| `flag_slot` | 响应 flag buffer slot | 同上 |
| `state` | PENDING / COMPLETED | 卡住的是否一直是 PENDING |
| `final_status` | sub-batch 最终状态 | 具体错误码和消息 |

### 常见诊断场景

| 你想查的问题 | 看哪列 |
|---|---|
| 拆 batch 是否正确（10 → 4/4/2） | `key_range` + `size` |
| cid 是否连续 | `cid` |
| 连接选择是否异常 | `channel` |
| buffer 生命周期（失败后是否释放） | `send_slot` / `flag_slot` |
| 失败传播（task 是否 PARTIAL_FAILED） | 表头 `task_status` + 各行 `final_status` |
| sub-batch 卡住不完成 | `state` 是否一直 PENDING |

---

## 手动 API（测试里用）

如果不想靠环境变量，也可以在测试里手动调用：

```cpp
#include "sub_batch_trace.h"

// 捕获快照
auto snap = UC::ASU::trace::CaptureTraceSnapshot(ctx);

// 打印到 ostream
UC::ASU::trace::PrintTraceTable(std::cout, snap);

// 返回字符串
std::string s = UC::ASU::trace::FormatTrace(ctx);

// 一步到位
UC::ASU::trace::DumpTrace(std::cout, ctx);
```
