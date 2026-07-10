import ucm.integration.vllm.patch.scheduler_metrics_patch  # noqa: F401
import ucm.integration.vllm.patch.v0191.vllm_ascend.ucm_connector_patch  # noqa: F401
from ucm.integration.vllm.patch.utils import patch_or_inject, when_imported
from ucm.logger import init_logger

logger = init_logger(__name__)


@when_imported("vllm_ascend.attention.mla_v1")
def patch_mla_v1(mod):
    logger.info(f"Patched {mod} MLA forward")

    from ucm.integration.vllm.patch.v0191.vllm_ascend.pc.attention import mla_v1

    patch_or_inject(mod.AscendMLAImpl, "forward", mla_v1.AscendMLAImpl.forward)


@when_imported("vllm_ascend.patch.platform.patch_mamba_config")
def patch_platform_mamba_config(mod):
    logger.debug(f"Patched {mod} called")

    from ucm.integration.vllm.patch.v0191.vllm_ascend.pc.platform import (
        patch_mamba_config,
    )

    patch_mamba_config.patch_hybrid_attention_mamba_model_config()
