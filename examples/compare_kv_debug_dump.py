import argparse
import sys

import torch


def iter_tensors(value):
    if isinstance(value, torch.Tensor):
        yield "tensor", value
    elif isinstance(value, (list, tuple)):
        for i, tensor in enumerate(value):
            yield str(i), tensor
    else:
        raise TypeError(f"Unsupported dumped value type: {type(value)}")


def compare_tensor(left, right, atol, rtol):
    left = left.float()
    right = right.float()
    diff = (left - right).abs()
    return {
        "shape": tuple(left.shape),
        "max_abs_diff": diff.max().item() if diff.numel() else 0.0,
        "mean_abs_diff": diff.mean().item() if diff.numel() else 0.0,
        "allclose": torch.allclose(left, right, atol=atol, rtol=rtol),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("store_dump", help="store_before *.pt file")
    parser.add_argument("load_dump", help="load_after *.pt file")
    parser.add_argument("--atol", type=float, default=0.0)
    parser.add_argument("--rtol", type=float, default=0.0)
    args = parser.parse_args()

    store = torch.load(args.store_dump, map_location="cpu")
    load = torch.load(args.load_dump, map_location="cpu")

    print(f"store phase: {store.get('phase')}, load phase: {load.get('phase')}")
    print(f"store blocks: {store.get('blocks')}")
    print(f"load blocks:  {load.get('blocks')}")

    common_layers = sorted(set(store["layers"]) & set(load["layers"]))
    missing_layers = sorted(set(store["layers"]) ^ set(load["layers"]))
    mismatches = 0
    checked = 0

    for layer_name in missing_layers:
        print(f"MISSING_LAYER layer={layer_name}")
        mismatches += 1

    for layer_name in common_layers:
        store_layer = store["layers"][layer_name]
        load_layer = load["layers"][layer_name]
        common_blocks = sorted(set(store_layer) & set(load_layer))
        missing_blocks = sorted(set(store_layer) ^ set(load_layer))
        for block_id in missing_blocks:
            print(f"MISSING_BLOCK layer={layer_name} block={block_id}")
            mismatches += 1

        for block_id in common_blocks:
            store_tensors = dict(iter_tensors(store_layer[block_id]))
            load_tensors = dict(iter_tensors(load_layer[block_id]))
            missing_tensors = sorted(set(store_tensors) ^ set(load_tensors))
            for tensor_name in missing_tensors:
                print(
                    f"MISSING_TENSOR {layer_name} block={block_id} "
                    f"tensor={tensor_name}"
                )
                mismatches += 1

            for tensor_name in sorted(set(store_tensors) & set(load_tensors)):
                result = compare_tensor(
                    store_tensors[tensor_name],
                    load_tensors[tensor_name],
                    args.atol,
                    args.rtol,
                )
                checked += 1
                if not result["allclose"]:
                    mismatches += 1
                print(
                    f"{layer_name} block={block_id} tensor={tensor_name} "
                    f"shape={result['shape']} "
                    f"max_abs_diff={result['max_abs_diff']:.8g} "
                    f"mean_abs_diff={result['mean_abs_diff']:.8g} "
                    f"allclose={result['allclose']}"
                )

    print(f"checked={checked}, mismatches={mismatches}")
    if mismatches:
        print("KV_CHECK_FAILED")
        sys.exit(1)
    print("KV_CHECK_PASSED")


if __name__ == "__main__":
    main()
