# UCM KV Debug Dump 使用说明

这个调试开关用于在 vLLM UCM connector 的 store/load 路径上保存 KV cache 快照，方便对比：

- `store_before`：调用 `dump_data` 之前，从 vLLM paged KV cache 中取出的 tensor。
- `load_after`：调用 `load_data` 并 `wait` 成功之后，从 vLLM paged KV cache 中取出的 tensor。

保存出来的是 PyTorch Tensor 的 CPU 快照，不是 ASU 后端原始文件，也不是指针地址。

## 1. 开启 dump

运行示例脚本前设置环境变量：

```powershell
$env:UCM_KV_DEBUG_DUMP_DIR="D:\a_storage\3_debug\kv_dump"
$env:UCM_KV_DEBUG_MAX_BLOCKS="2"
$env:UCM_KV_DEBUG_MAX_LAYERS="2"
python examples\offline_llm_qwen3.py
```

环境变量说明：

```text
UCM_KV_DEBUG_DUMP_DIR      dump 文件输出目录。不设置则不会 dump。
UCM_KV_DEBUG_MAX_BLOCKS   每次最多 dump 几个 block，默认 2。
UCM_KV_DEBUG_MAX_LAYERS   每次最多 dump 几层，默认 0，表示所有层。
```

建议先只 dump 少量 block 和 layer，例如 `2 blocks + 2 layers`，否则文件会很大。

## 2. 生成文件

输出目录里会生成类似文件：

```text
000001_store_before_rank0_reqxxx.pt
000002_store_before_rank1_reqxxx.pt
000003_load_after_rank0_reqxxx.pt
000004_load_after_rank1_reqxxx.pt
```

命名含义：

```text
store_before  store/dump_data 之前的 KV tensor 快照
load_after    load/load_data 完成之后的 KV tensor 快照
rank0/rank1   tensor parallel rank
reqxxx        request id
```

## 3. 校验 store 和 load

使用仓库中的校验脚本：

```powershell
python examples\compare_kv_debug_dump.py `
  D:\a_storage\3_debug\kv_dump\000001_store_before_rank0_reqxxx.pt `
  D:\a_storage\3_debug\kv_dump\000003_load_after_rank0_reqxxx.pt
```

重点看最后几行：

```text
checked=xxx, mismatches=0
KV_CHECK_PASSED
```

如果通过，脚本退出码为 `0`，说明 `.pt` 文件中的 tensor 数值一致。

如果失败，会输出类似：

```text
max_abs_diff=...
mean_abs_diff=...
allclose=False
checked=xxx, mismatches=1
KV_CHECK_FAILED
```

失败时脚本退出码为 `1`。这通常说明 store/load 或写回 vLLM KV buffer 的过程可能有问题。

如果 KV 数值一致但模型输出仍然乱码，则问题可能不在 KV 内容本身，而更可能在 block 映射、命中 token 数、position/rope、调度复用边界，或者输入文本编码。

如果需要允许少量浮点误差，可以指定容忍度：

```powershell
python examples\compare_kv_debug_dump.py `
  D:\a_storage\3_debug\kv_dump\000001_store_before_rank0_reqxxx.pt `
  D:\a_storage\3_debug\kv_dump\000003_load_after_rank0_reqxxx.pt `
  --atol 1e-5 `
  --rtol 1e-5
```

## 4. 查看具体 tensor

查看某个 dump 文件里的 layer、block、shape、dtype 和前 100 个数值：

```powershell
python -c "import torch; x=torch.load(r'D:\a_storage\3_debug\kv_dump\000001_store_before_rank0_reqxxx.pt', map_location='cpu'); layer=next(iter(x['layers'])); block=next(iter(x['layers'][layer])); t=x['layers'][layer][block]; print(layer); print(block); print(t.shape, t.dtype); print(t.flatten()[:100])"
```

如果 dump 的 KV cache 是 tuple/list 形式，可以这样看：

```powershell
python -c "import torch; x=torch.load(r'D:\a_storage\3_debug\kv_dump\000001_store_before_rank0_reqxxx.pt', map_location='cpu'); layer=next(iter(x['layers'])); block=next(iter(x['layers'][layer])); v=x['layers'][layer][block]; print(type(v)); print(v[0].shape, v[0].flatten()[:50]); print(v[1].shape, v[1].flatten()[:50])"
```

## 5. dump 文件结构

`.pt` 文件可以通过 `torch.load(..., map_location="cpu")` 读取，结构大致如下：

```python
{
    "phase": "store_before",
    "request_id": "...",
    "tp_rank": 0,
    "local_rank": 0,
    "block_size": 128,
    "hash_block_size": 128,
    "blocks": [
        {
            "ucm_block_id": "...",
            "vllm_block_id": 12,
        }
    ],
    "layers": {
        "layer_name": {
            "ucm_block_id_hex": tensor(...)
        }
    }
}
```

其中：

```text
ucm_block_id   UCM 使用的 block hash，十六进制字符串
vllm_block_id  vLLM paged KV cache 中的 block id
layers         每层对应的 KV tensor 快照
```

## 6. 注意事项

- dump 会从设备拷贝 tensor 到 CPU，并写入磁盘，会明显影响性能。
- 大模型、长上下文、全层 dump 会产生很大的 `.pt` 文件。
- 多 TP rank 时，需要分别对比相同 rank 的 `store_before` 和 `load_after` 文件。
- 这个工具用于验证 vLLM KV buffer 中的数值是否一致，不直接验证 ASU 后端内部存储格式。
