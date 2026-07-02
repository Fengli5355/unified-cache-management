#!/bin/bash

# set the NPU device number
export ASCEND_RT_VISIBLE_DEVICES=0,1

# Set the operator dispatch pipeline level to 1 and disable manual memory control in ACLGraph
export TASK_QUEUE_ENABLE=1

# [Optional] jemalloc
# jemalloc is for better performance, if `libjemalloc.so` is installed on your machine, you can turn it on.
# if os is Ubuntu
# export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2:$LD_PRELOAD
# if os is openEuler
# export LD_PRELOAD=/usr/lib64/libjemalloc.so.2:$LD_PRELOAD

# # Enable the AIVector core to directly schedule ROCE communication
# export HCCL_OP_EXPANSION_MODE="AIV"

# # Enable FlashComm_v1 optimization when tensor parallel is enabled.
# export VLLM_ASCEND_ENABLE_FLASHCOMM1=1
# ref: https://docs.vllm.ai/projects/ascend/en/v0.18.0/tutorials/models/Qwen3-Dense.html

# 启动vLLM服务
# vllm serve /models/Qwen3-32B \
#   --served-model-name qwen3 \
#   --trust-remote-code \
#   --async-scheduling \
#   --distributed-executor-backend mp \
#   --tensor-parallel-size 2 \
#   --no-enable-prefix-caching \
#   --max-model-len 8000 \
#   --max-num-batched-tokens 8000 \
#   --kv-transfer-config '{"kv_connector":"UCMConnector","kv_connector_module_path":"ucm.integration.vllm.ucm_connector","kv_role":"kv_both","kv_connector_extra_config":{"UCM_CONFIG_FILE":"/home/zy/unified-cache-management/examples/ucm_config_asu.yaml"}}' \
#   --port 8113 \
#   --block-size 128 \
#   --gpu-memory-utilization 0.9


vllm serve /mnt/model/Qwen3-32B \
  --served-model-name qwen3 \
  --trust-remote-code \
  --async-scheduling \
  --distributed-executor-backend mp \
  --tensor-parallel-size 2 \
  --no-enable-prefix-caching \
  --max-model-len 8000 \
  --max-num-batched-tokens 8000 \
  --kv-transfer-config '{
    "kv_connector":"UCMConnector",
    "kv_connector_module_path":"ucm.integration.vllm.ucm_connector",
    "kv_role":"kv_both",
    "kv_connector_extra_config":{
      "UCM_CONFIG_FILE":"/home/zy/aiv_ucm/unified-cache-management/examples/ucm_config_asu.yaml"
    }
  }' \
  --port 8113 \
  --block-size 128 \
  --gpu-memory-utilization 0.9