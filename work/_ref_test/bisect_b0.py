# bisect_b0.py — reference intermediates for block 0 at 512x320 (torch CPU).
_REPO = Path(__file__).resolve().parents[2]
import pathlib

import numpy as np
import torch
from safetensors import safe_open

import mlxdlss.model as M

ROOT = pathlib.Path("" + str(_REPO) + "/work/_ref_test")
W = ROOT.parent / "mlxw" / "dlssnr-logical.safetensors"

feats = torch.from_numpy(np.fromfile(ROOT / "golden_features.bin", dtype=np.float32).reshape(1, 320, 512, 16))

with safe_open(str(W), framework="pt", device="cpu") as s:
    w = {n: s.get_tensor(n) for n in s.keys() if n.startswith("block0.layer0")}

ada = feats.reshape(1, -1, 16) @ w["block0.layer0.input_adapter_weight"].float()
ada = ada.reshape(1, 320, 512, 32)
ada.numpy().astype(np.float32).tofile(ROOT / "ref_ada.bin")

tokens = ada.reshape(1, -1, 32)
branch = M.e4m3_round_trip(M.quadratic_gate_activation(tokens @ w["block0.layer0.weight1"].float())) \
    @ w["block0.layer0.weight2"].float()
ffn_out = M.cosine_residual(tokens, branch, w["block0.layer0.ffn_cos_skip"].float())
ffn_out_nhwc = ffn_out.reshape(1, 320, 512, 32)
ffn_out_nhwc.numpy().astype(np.float32).tofile(ROOT / "ref_b0ffn.bin")

bias = w["block0.layer0.attn_bias"].float()
if M.uses_fragment_swizzle(0, 1):
    bias = M.recover_attention_bias_layout(bias)
attn = M.window_attention(
    ffn_out_nhwc,
    qkv_weight=w["block0.layer0.qkv_weight"].float(),
    attention_scale=w["block0.layer0.attn_scale"].float(),
    attention_bias=bias,
    projection_weight=w["block0.layer0.projection_weight"].float(),
    head_count=1,
    window_size=8,
    window_origin=(0, 0),
)
attn.numpy().astype(np.float32).tofile(ROOT / "ref_b0attn.bin")

w0 = M._residual_per_token(ffn_out_nhwc, attn, w["block0.layer0.attn_cos_skip"].float())
w0.numpy().astype(np.float32).tofile(ROOT / "ref_w0.bin")

golden = np.fromfile(ROOT / "golden_w0.bin", dtype=np.float32)
print("w0 vs golden max:", np.abs(w0.numpy().astype(np.float32).reshape(-1) - golden).max())
print("ok")
