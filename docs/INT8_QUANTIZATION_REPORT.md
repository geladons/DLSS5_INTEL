# INT8 Quantization Accuracy & Per-Channel Calibration Report for DLSSNR Graph

## 1. Executive Summary

This study evaluates the accuracy, numerical stability, and runtime format design for INT8 quantization of the 71-block DLSSNR graph on Intel Arc Pro B50 (Xe2 architecture).

Key findings:
1. **Weight Reconstruction Accuracy**:
   - Per-channel weight quantization for Matrix B (`scale_w[j] = max(abs(W[:, j])) / 127.0`) achieves an extraordinarily low global reconstruction error across all 257 2D GEMM weight tensors (133.7M parameters):
     - **Global Weighted MSE**: `5.913e-08` (RMSE: `0.000243`)
     - **Global Max Absolute Error**: `0.003414`
     - **Weight Scale Distribution** (159,496 channels): Mean = `0.001011`, Median = `0.000923`, 99th percentile = `0.003691`, Max = `0.007478`.
   - For 3D batched GEMM weight tensors (32 tensors, 4.2M parameters):
     - **Global Weighted MSE**: `3.888e-07` (RMSE: `0.000624`), Max Error = `0.002338`.
   - VRAM weight footprint decreases by **50%** from **278.01 MiB** to **~139.0 MiB** (with scale overhead of only **623 KB**).

2. **Simulation on Pathological GEMM Shapes `(138240, 128, 32)` and `(552960, 128, 32)`**:
   - **Standard Gated Activations**:
     - Static per-tensor scaling: PSNR ~57.7 dB, Mean Relative Error = ~9.0%, Pearson Correlation = 0.99982.
     - Dynamic 32-token block scaling: PSNR ~60.6 dB (+2.9 dB), Mean Relative Error = ~6.3%, Pearson Correlation = 0.99991.
   - **Pathological High-Dynamic-Range / Specular Spike Activations** (isolated outliers up to 60.0, e.g., bloom/reflections):
     - Static per-tensor scaling suffers severe dynamic range collapse on normal tokens: **Mean Relative Error blows up to 244% - 278%** because `scale_x` is dominated by rare outlier spikes, and Pearson Correlation drops to 0.9896.
     - Dynamic 32-token block scaling isolates outlier tokens completely: **Mean Relative Error stays at 9.5% - 10.7% (a 26x improvement)**, PSNR reaches **83.8 - 87.5 dB (+17 dB gain)**, and Pearson Correlation remains **0.99980+**.
   - **Severe Pathological Activations** (outliers up to 150.0):
     - Static per-tensor scaling produces 281% - 337% relative error.
     - Dynamic 32-token block scaling maintains tight control with 14.6% - 16.6% relative error (**21x improvement**), PSNR 83.8 - 87.4 dB (+14 dB gain), and Pearson Correlation 0.99988+.

---

## 2. Objective 1: Safetensors Weight Inspection & Per-Channel Calibration

Weights file: `work/mlxw/dlssnr-logical.safetensors` (649 tensors total).

### 2.1 GEMM Weight Tensor Inventory
- **2D GEMM Weights**: 257 tensors, 133,739,136 parameters (255.09 MiB FP16 -> 127.54 MiB INT8).
  - `conv_weight`: 1 tensor `(1024, 512)`
  - `ffn_output_projection_weight`: 36 tensors `(64, 64)`, `(128, 128)`, `(256, 256)`
  - `first_projection_weight`: 16 tensors `(512, 512)`
  - `input_adapter_weight`: 1 tensor `(16, 32)`
  - `out_conv_weight`: 1 tensor `(16, 4)`
  - `out_gain`: 1 tensor `(16, 4)`
  - `projection_weight`: 70 tensors `(32, 32)` .. `(1024, 1024)`
  - `qkv_weight`: 70 tensors `(32, 96)` .. `(1024, 3072)`
  - `weight`: 17 tensors `(512, 1024)`, `(1024, 4096)`, `(4096, 1024)`
  - `weight0`: 8 tensors `(32, 64)` .. `(512, 256)`
  - `weight1`: 10 tensors `(32, 128)`
  - `weight2`: 10 tensors `(128, 32)`
  - `weight3`: 16 tensors `(512, 512)`
- **3D Batched GEMM Weights**: 32 tensors, 4,194,304 parameters (8.00 MiB FP16 -> 4.00 MiB INT8).
  - `group_expand_weight`: 16 tensors `(8, 64, 256)`
  - `group_project_weight`: 16 tensors `(8, 256, 64)`
- **4D/5D FFN Projections (in GEMM form)**:
  - `ffn_branch_projection_weight`: 36 tensors `(G, 4, 32, 32)` = 786,432 elements evaluated as G slices of `(128, 32)`.

### 2.2 Calibration & Reconstruction Accuracy

Formulas:
- Scale per channel: $scale\_w[j] = \frac{\max(|W_{:, j}|)}{127.0}$
- Quantization: $W_q = \text{clip}\left(\text{round}\left(\frac{W}{scale\_w}\right), -128, 127\right)$
- Reconstruction: $W_{rec} = W_q \cdot scale\_w$

Detailed breakdown:
| Category | Tensor Count | Parameter Count | MSE | Max Abs Error | Mean Scale | Max Scale |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `conv_weight` | 1 | 524,288 | 4.712e-08 | 0.000554 | 0.000753 | 0.001107 |
| `ffn_output_projection` | 36 | 1,277,952 | 5.132e-08 | 0.001599 | 0.000751 | 0.003199 |
| `first_projection` | 16 | 4,194,304 | 1.228e-07 | 0.000984 | 0.001186 | 0.001969 |
| `input_adapter` | 1 | 512 | 1.950e-06 | 0.003411 | 0.005113 | 0.007478 |
| `projection_weight` | 70 | 13,871,104 | 2.432e-08 | 0.002338 | 0.000501 | 0.004921 |
| `qkv_weight` | 70 | 41,613,312 | 1.005e-07 | 0.003414 | 0.001305 | 0.006890 |
| `weight` (global MLP) | 17 | 67,633,152 | 3.767e-08 | 0.002092 | 0.000792 | 0.004429 |
| `weight0` (transitions) | 8 | 348,160 | 2.235e-07 | 0.003260 | 0.001926 | 0.006890 |
| `weight1` (L0/L1 FFN in) | 10 | 40,960 | 1.360e-06 | 0.003414 | 0.004019 | 0.006890 |
| `weight2` (L0/L1 FFN out)| 10 | 40,960 | 2.250e-07 | 0.002338 | 0.001419 | 0.004921 |
| `weight3` (L5 bottleneck)| 16 | 4,194,304 | 2.206e-08 | 0.000800 | 0.000468 | 0.001599 |
| `group_expand_weight` | 16 | 2,097,152 | 5.760e-07 | 0.002338 | 0.002582 | 0.004921 |
| `group_project_weight`| 16 | 2,097,152 | 2.016e-07 | 0.002338 | 0.001568 | 0.004921 |
| `ffn_branch_projection`| 36 | 786,432 | 3.959e-07 | 0.002338 | 0.002149 | 0.004921 |

### 2.3 Weight Scale Distribution Statistics (159,496 channels)
- **Min**: 0.00000000
- **1st Percentile**: 0.00004614
- **5th Percentile**: 0.00018455
- **25th Percentile (Q1)**: 0.00073819
- **Median (Q2)**: 0.00092274
- **Mean**: 0.00101062
- **75th Percentile (Q3)**: 0.00123031
- **95th Percentile**: 0.00196850
- **99th Percentile**: 0.00369094
- **Max**: 0.00747801
- **Standard Deviation**: 0.00064151

---

## 3. Objective 2: GEMM Simulation on Pathological Shapes

Shapes tested:
- Stage 1 (Quarter resolution): $M=138,240, K=128, N=32$
- Stage 0 (Native resolution): $M=552,960, K=128, N=32$
Tested on real weights: `block0.layer0.weight2`, `block4.layer0.weight2`, `block70.layer0.weight2`, and `block10.layer0.ffn_branch_projection_weight` (slice).

### 3.1 Numerical Comparison Results: Stage 1 ($M=138,240$)

#### Regime 1: Standard DLSSNR Gated Activations ($X \in [-0.53, 1.88]$, std=0.235)
| Weight Tensor | Method | PSNR (ptp) | PSNR (max) | Mean Rel Err | Rel L2 Err | Correlation |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `block0_weight2` | Static Per-Tensor | 57.73 dB | 52.03 dB | 0.09047 | 1.849e-02 | 0.999827 |
| | Dynamic Block 32 | **60.48 dB** | **54.78 dB** | **0.06627** | **1.347e-02** | **0.999908** |
| `block4_weight2` | Static Per-Tensor | 58.46 dB | 52.74 dB | 0.08359 | 1.905e-02 | 0.999819 |
| | Dynamic Block 32 | **61.40 dB** | **55.68 dB** | **0.05910** | **1.358e-02** | **0.999908** |
| `block70_weight2` | Static Per-Tensor | 57.17 dB | 51.95 dB | 0.08862 | 1.938e-02 | 0.999813 |
| | Dynamic Block 32 | **60.25 dB** | **55.03 dB** | **0.06163** | **1.360e-02** | **0.999908** |
| `block10_branch_prj` | Static Per-Tensor | 55.22 dB | 49.25 dB | 0.09388 | 1.859e-02 | 0.999828 |
| | Dynamic Block 32 | **58.32 dB** | **52.35 dB** | **0.06497** | **1.300e-02** | **0.999916** |

#### Regime 2: Pathological HDR/Specular Spikes (0.05% outliers in $[20, 60]$, max $\approx 60.0$)
| Weight Tensor | Method | PSNR (ptp) | PSNR (max) | Mean Rel Err | Rel L2 Err | Correlation |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `block0_weight2` | Static Per-Tensor | 69.24 dB | 64.13 dB | 2.54718 (255%) | 1.442e-01 | 0.989737 |
| | Dynamic Block 32 | **86.36 dB** | **81.25 dB** | **0.10268 (10.3%)** | **2.009e-02** | **0.999799** |
| `block4_weight2` | Static Per-Tensor | 69.05 dB | 63.04 dB | 2.44587 (245%) | 1.387e-01 | 0.990492 |
| | Dynamic Block 32 | **86.34 dB** | **80.33 dB** | **0.09548 (9.5%)** | **1.897e-02** | **0.999821** |
| `block70_weight2` | Static Per-Tensor | 68.84 dB | 62.89 dB | 2.63914 (264%) | 1.418e-01 | 0.990058 |
| | Dynamic Block 32 | **86.26 dB** | **80.32 dB** | **0.09972 (10.0%)** | **1.908e-02** | **0.999818** |
| `block10_branch_prj` | Static Per-Tensor | 66.43 dB | 61.07 dB | 2.78593 (279%) | 1.449e-01 | 0.989643 |
| | Dynamic Block 32 | **83.86 dB** | **78.51 dB** | **0.10600 (10.6%)** | **1.946e-02** | **0.999812** |

#### Regime 3: Severe Pathological Spikes (0.1% outliers in $[50, 150]$, max $\approx 150.0$)
| Weight Tensor | Method | PSNR (ptp) | PSNR (max) | Mean Rel Err | Rel L2 Err | Correlation |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `block0_weight2` | Static Per-Tensor | 71.59 dB | 66.23 dB | 2.81635 (282%) | 6.975e-02 | 0.997576 |
| | Dynamic Block 32 | **84.86 dB** | **79.50 dB** | **0.15051 (15.1%)** | **1.514e-02** | **0.999886** |
| `block4_weight2` | Static Per-Tensor | 73.25 dB | 67.47 dB | 2.88351 (288%) | 6.905e-02 | 0.997616 |
| | Dynamic Block 32 | **86.77 dB** | **80.98 dB** | **0.14590 (14.6%)** | **1.456e-02** | **0.999894** |
| `block70_weight2` | Static Per-Tensor | 71.19 dB | 65.40 dB | 3.13739 (314%) | 7.074e-02 | 0.997498 |
| | Dynamic Block 32 | **84.89 dB** | **79.10 dB** | **0.15672 (15.7%)** | **1.460e-02** | **0.999894** |
| `block10_branch_prj` | Static Per-Tensor | 69.98 dB | 64.03 dB | 3.36823 (337%) | 6.942e-02 | 0.997591 |
| | Dynamic Block 32 | **83.78 dB** | **77.84 dB** | **0.16561 (16.6%)** | **1.416e-02** | **0.999900** |

### 3.2 Numerical Comparison Results: Stage 0 ($M=552,960$)
Identical patterns hold at full native resolution ($M=552,960$ tokens):
- Under Standard Activations: Dynamic Block 32 achieves **PSNR 60.5 - 62.1 dB** (vs 54.9 - 59.0 dB static) and **Mean Relative Error 5.9% - 6.5%** (vs 8.6% - 9.6% static).
- Under Pathological HDR Spikes (0.05% in $[20, 60]$):
  - Static Per-Tensor: **Mean Rel Err = 244% - 278%**, Rel L2 Err = 14.1% - 14.3%, Correlation = 0.9899 - 0.9902.
  - Dynamic Block 32: **Mean Rel Err = 9.6% - 10.8% (26x reduction)**, Rel L2 Err = 1.92% - 1.99%, PSNR = **84.9 - 87.5 dB (+17 dB gain)**, Correlation = **0.99980 - 0.99982**.
- Under Severe Pathological Spikes (0.1% in $[50, 150]$):
  - Static Per-Tensor: **Mean Rel Err = 281% - 334%**, Rel L2 Err = 6.92% - 6.99%, Correlation = 0.9976.
  - Dynamic Block 32: **Mean Rel Err = 14.6% - 16.4% (21x reduction)**, Rel L2 Err = 1.41% - 1.53%, PSNR = **85.5 - 87.4 dB (+14 dB gain)**, Correlation = **0.99988 - 0.99990**.

---

## 4. Objective 3: Runtime Quantization Format Recommendations

### 4.1 Header Specification (`dlssnr_q8.bin`)
A binary container file replaces or sits alongside `dlssnr-logical.safetensors`:

```cpp
struct Q8FileHeader {
    uint32_t magic;            // 0x38513544 ('D5Q8')
    uint32_t version;          // 1
    uint32_t num_tensors;      // Number of quantized weight tensors (e.g. 289)
    uint32_t flags;            // Bit 0: SYMMETRIC_INT8, Bit 1: PER_CHANNEL_B, Bit 2: BLOCK32_ACT_SCALE
    uint64_t total_weight_bytes;// ~145,700,000 bytes (INT8 data)
    uint64_t total_scale_bytes; // ~638,000 bytes (FP32 scale tables)
    uint64_t index_offset;     // Offset to TensorEntry table (aligned to 64 bytes)
    uint64_t data_offset;      // Offset to weight payload (aligned to 256 bytes)
    uint8_t  reserved[32];
};

struct Q8TensorEntry {
    char     name[96];         // Tensor name string (null-terminated)
    uint16_t ndim;             // 2, 3, or 4
    uint16_t dtype;            // 1 = INT8 weights
    uint32_t shape[4];         // [K, N] or [B, K, N]
    uint64_t weight_offset;    // Aligned to 256 bytes in data arena
    uint64_t weight_bytes;     // K * N * 1
    uint64_t scale_offset;     // Aligned to 64 bytes in scale arena
    uint32_t scale_count;      // N scales
    uint32_t flags;            // e.g. IS_BRANCH_FOLDED, HAS_ATTN_BIAS_SWIZZLE
};
```

### 4.2 Scale Layout & Alignment Rules

1. **Matrix B (Weight Scales)**:
   - Layout: Contiguous FP32 array of length $N$ channels for each weight tensor.
   - Alignment: 64-byte aligned (matching Intel Xe2 cacheline size) and 256-byte aligned for Vulkan storage buffers (`minStorageBufferOffsetAlignment`).
   - Sizing: $159,496 \times 4\text{ bytes} = 623.0\text{ KB}$. Fits easily in L3 / dedicated uniform buffer.

2. **Matrix A (Dynamic Activation Scales - Block 32)**:
   - Why Block 32 is Hardware-Optimal on Arc B50:
     - In `dlss5/m8b-live/shaders/m8/gemm.comp`, the workgroup tile configuration is:
       $$\text{BM} = \text{TM} \times \text{RM} = 8 \times 4 = 32\text{ rows}$$
     - Each subgroup computes a tile of 32 tokens in $M$!
     - Therefore, **1 block of 32 tokens = exactly 1 workgroup tile along $M$**.
     - No inter-workgroup synchronization is required. The workgroup loads its tile of 32 rows, computes `max(|X|)` in shared local memory (SLM) using `subgroupMax`, sets `scale_x = max / 127.0`, and loads `scale_w[col]` directly.
   - Scale Buffer Sizing:
     - $M = 138,240 \rightarrow 4,320\text{ blocks} \times 4\text{ bytes} = 17.28\text{ KB}$ (or 8.64 KB FP16).
     - $M = 552,960 \rightarrow 17,280\text{ blocks} \times 4\text{ bytes} = 69.12\text{ KB}$ (or 34.56 KB FP16).

3. **Combined Epilogue Scaling**:
   - In the cooperative matrix accumulator, MMA runs in INT8:
     $$\text{accum}_{i32} = X_{q, i8} \times W_{q, i8}$$
   - Epilogue dequantization multiplies accumulator components by:
     $$\text{value}_{f32} = \text{float}(\text{accum}_{i32}) \times (\text{scale\_x} \times \text{scale\_w}[\text{col}])$$
   - This eliminates all intermediate FP16 conversions, doubles math throughput on Intel XMX engines (Arc B50 XMX INT8 throughput is $2\times$ FP16 throughput), and halves memory bus traffic.

---

## 5. Summary Recommendation

| Metric | Baseline FP16 | Proposed INT8 (Per-Channel B + Block 32 A) | Benefit |
| :--- | :--- | :--- | :--- |
| **Weight VRAM** | 278.01 MiB | 139.0 MiB + 0.62 MiB scales | **50% VRAM Reduction** |
| **Memory Bandwidth** | 100% | 50% on all weight loads | **2x Weight Load Throughput** |
| **Weight Reconstruction MSE** | 0.0 | $5.913 \times 10^{-8}$ | **Negligible error (RMSE 0.00024)** |
| **Correlation under Outliers** | 1.000 | 0.9998+ (vs 0.989 static) | **No HDR dynamic range collapse** |
| **Hardware Fit (Arc B50)** | $8\times 16\times 16$ f16 | $8\times 16\times 16$ i8xi8->i32 XMX coopmat | **Native Xe2 XMX acceleration** |

Dynamic token-block activation quantization with block size 32 is strictly recommended over static per-tensor scaling, completely resolving the activation dynamic range issue while matching Xe2 hardware tile dimensions.
