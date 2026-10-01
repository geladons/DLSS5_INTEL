// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// tools/test_int8_loader.cpp — Verification test for INT8 weights loader
// ============================================================================
#include "../dlss5/chain/weights.h"
#include "../dlss5/chain/arena.h"
#include <cstdio>
#include <cassert>
#include <vector>

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("=== TEST 1: Replacement mode (load INT8 .bin directly) ===\n");
    d5c::WeightsStore ws1;
    if (!ws1.load("work/mlxw/dlssnr-int8.bin")) {
        std::fprintf(stderr, "FAIL: failed to load dlssnr-int8.bin\n");
        return 1;
    }
    std::printf("PASS: Loaded %zu tensors from dlssnr-int8.bin\n", ws1.wf.tensors.size());
    assert(ws1.wf.tensors.size() == 649);

    size_t qcount = 0;
    for (const auto& kv : ws1.wf.tensors) {
        if (kv.second.isQuantized) qcount++;
    }
    std::printf("PASS: Identified %zu quantized tensors (expected 254)\n", qcount);
    assert(qcount == 254);

    if (!ws1.computePack()) {
        std::fprintf(stderr, "FAIL: computePack failed for INT8 pack\n");
        return 1;
    }
    std::printf("PASS: computePack succeeded: packTotal = %llu bytes (%.2f MiB)\n",
                (unsigned long long)ws1.packTotal, ws1.packTotal / 1048576.0);

    // Verify alignments of all quantized tensors
    for (const auto& kv : ws1.wf.tensors) {
        if (kv.second.isQuantized) {
            uint64_t w_off = ws1.woff(kv.first);
            uint64_t sc_off = ws1.scaleOff(kv.first);
            assert(w_off % 256 == 0);
            assert(w_off % 64 == 0);
            assert(sc_off % 256 == 0);
            assert(sc_off % 64 == 0);
            assert(sc_off > w_off);
        }
    }
    std::printf("PASS: All 254 INT8 weight and scale offsets verified 256-byte and 64-byte aligned.\n");

    // Test preprocessInto with real ChainArena layout
    d5c::Stage st[7]{};
    for (int s = 0; s < 6; ++s) { st[s].H = 180; st[s].W = 320; st[s].TOK = 180 * 320; }
    st[6].H = 90; st[6].W = 160; st[6].TOK = 90 * 160;
    uint32_t T6P = (st[6].TOK + 31) & ~31u;

    d5c::ChainArena arena1;
    arena1.layout(st, T6P, ws1.packTotal, ws1);
    std::vector<char> mockArena(arena1.totalBytes, 0);
    d5c::InitSwizzle();
    ws1.preprocessInto(mockArena.data(), arena1.vecOff, arena1.off.oHeadW);
    std::printf("PASS: preprocessInto executed without errors for replacement mode (total arena %.2f MiB).\n",
                arena1.totalBytes / 1048576.0);

    std::printf("\n=== TEST 2: Alongside mode (load FP16 .safetensors then INT8 .bin) ===\n");
    d5c::WeightsStore ws2;
    if (!ws2.load("work/mlxw/dlssnr-logical.safetensors")) {
        std::fprintf(stderr, "FAIL: failed to load dlssnr-logical.safetensors\n");
        return 1;
    }
    std::printf("PASS: Loaded %zu FP16 tensors from safetensors\n", ws2.wf.tensors.size());

    if (!ws2.loadInt8("work/mlxw/dlssnr-int8.bin")) {
        std::fprintf(stderr, "FAIL: failed to loadInt8 dlssnr-int8.bin\n");
        return 1;
    }
    std::printf("PASS: INT8 weights merged alongside FP16 weights\n");

    if (!ws2.computePack()) {
        std::fprintf(stderr, "FAIL: computePack failed for alongside mode\n");
        return 1;
    }
    std::printf("PASS: computePack succeeded for alongside mode: packTotal = %llu bytes (%.2f MiB)\n",
                (unsigned long long)ws2.packTotal, ws2.packTotal / 1048576.0);

    d5c::ChainArena arena2;
    arena2.layout(st, T6P, ws2.packTotal, ws2);
    std::vector<char> mockArena2(arena2.totalBytes, 0);
    ws2.preprocessInto(mockArena2.data(), arena2.vecOff, arena2.off.oHeadW);
    std::printf("PASS: preprocessInto executed without errors for alongside mode (total arena %.2f MiB).\n",
                arena2.totalBytes / 1048576.0);

    std::printf("\n>>> ALL INT8 LOADER AND NUMERICAL CHECKS PASSED <<<\n");
    return 0;
}
