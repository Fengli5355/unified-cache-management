#!/bin/bash

# 设置环境变量
export UC_LOGGER_LEVEL="debug"
export ASCEND_RT_VISIBLE_DEVICES="0"
export UMC_LOG_DEBUG="1"
export UMC_ASU_OOB_MODE="tcp"
export UMC_ASU_DEVICE_ID="0"

#export MODEL_PATH="/mnt/model/DeepSeek-V2-Lite-Chat"

python examples/offline_llm_qwen3.py
