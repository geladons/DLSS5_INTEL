// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain — ChainEngine: the full absolute (echo-free) DLSSNR pipeline.
//   BGRA frame in -> decode -> vendor features -> featpack -> 71-block chain
//   -> vendor compose_head -> encode -> BGRA frame out.
// Owns the Vulkan context, weights residency, chunked arena, chain recorder
// and the M9b front-end buffers/descriptors. No DDA, no overlay, no present —
// used by m11d (daemon) and by the m8b-live echo-free path.
// Extracted from dlss5/m8b-live/main.cpp sections 5-11 + the frame record.
// ============================================================================
#pragma once

#include "vk_util.h"
#include "vk_context.h"
#include "weights.h"
#include "arena.h"
#include "recorder.h"
#include "prof.h"
#include "phase30.h"

#include <string>
#include <chrono>

namespace d5c {

struct EngineConfig {
    uint32_t canvasW = 0, canvasH = 0;                 // frame size
    uint32_t regionLeft = 0, regionTop = 0;            // processing region within the canvas
    uint32_t regionW = 0, regionH = 0;                 // (0 = whole canvas)
    std::string weightsPath;                           // dlssnr-logical.safetensors
    std::string shaderDir;                             // dir with *.spv (trailing slash)
    float headGain = 1.0f;                             // vendor residual scale (--gain)
    float headBlend = 1.0f;                            // vendor mix factor / blend knob home
                                                       // (public baseline 1.0). The LIVE compose
                                                       // mix slot (pc.c.w) is phase30.intensity -
                                                       // kept strictly separate from headBlend.
    float renderScale = 1.0f;
    float sharpness = 0.8f;
    bool temporal = true;                              // vendor history conditioning (features 7-9
                                                       // + temporal_nr compose). Damps per-frame
                                                       // reconstruction jitter on moving content.
    Phase30Params phase30{};                           // NVIDIA Phase 30 condition parameters
    // Gain anchor: EMA smoothing factor for the GLOBAL mean gain
    // (processed-luma / input-luma). The frame's output gain is forced to
    // the smoothed value, so network exposure "breathing" and per-frame
    // gain jitter are low-passed away while real lighting changes track
    // within ~1/alpha frames (>40% jumps snap instantly - scene cuts).
    // Requires temporal_stats.spv alongside temporal_nr.spv.
    // DEFAULT ON (0.08 = ~12-frame EMA window): the BOUNDED anchor
    // (6% deadzone + 0.80..1.25 clamp in temporal_nr.comp) is the
    // shipping configuration - the field-verified DC-breathing fix
    // (frame luma delta 5.39 -> 0.09). Opt out via
    // DLSS5_GAIN_LOCK / NR_LAYER_GAIN_LOCK / D5C_GAIN_LOCK = 0.
    float gainLock = 0.08f;
    VkContextConfig vkCfg{};                           // device selection (headless default)
    bool debugDumps = false;                           // dump live_*.bin on frame index 1
    std::string dumpDir = "out";                       // dump destination
    std::string profPath;                              // M10: per-dispatch CSV (empty = off)
};

struct FrameStats {
    double gpuTotalMs = 0, feMs = 0, fpMs = 0, chainMs = 0, tailMs = 0;
    double wallMs = 0;
};

class ChainEngine {
public:
    bool init(const EngineConfig& cfg);       // heavy: device + weights + arena (~2 s)
    // bgraIn/bgraOut: canvasW*canvasH*4 bytes. frameIndex only gates debug
    // dumps/profiling; the vendor noise seed is pinned (engine.cpp b[0]=1)
    // so identical input always yields identical output.
    bool processFrame(const uint8_t* bgraIn, uint8_t* bgraOut, long frameIndex,
                      FrameStats* stats = nullptr);
    bool interpolateFrame(uint32_t w, uint32_t h, const void* f0, const void* f1,
                          void* out, float factor = 0.5f, FrameStats* stats = nullptr);
    // 1:2 frame generation: builds a generated frame from the current raw
    // present using the engine's own copy of the last processed frame and its
    // raw input (residual transfer along raw->raw motion). Only curRaw is
    // uploaded; the prev processed/raw pair is refreshed by processFrame.
    bool transferFrame(uint32_t w, uint32_t h, const void* curRaw,
                       void* out, FrameStats* stats = nullptr);
    // Mode-2 frame generation (+1 present of latency): synthesizes the
    // midpoint between the two most recent processed anchors using the
    // real captured middle frame as the confidence fallback. midRaw may
    // be NULL (unwarped anchor blend fallback instead). Needs >=1 anchor;
    // with <2 anchors it copies the newest processed anchor - always a
    // presentable, on-time frame.
    bool interpolateAnchors(uint32_t w, uint32_t h, const void* midRaw,
                            void* out, float factor = 0.5f,
                            FrameStats* stats = nullptr);
    // Frame-generation mode: 1 = residual transfer (+0 latency), 2 =
    // anchor interpolation (+1 frame). Setting 2 without the fginterp
    // pipeline available clamps to 1; a mode switch drops the anchors.
    void setFrameGenMode(int mode);
    int frameGenMode() const { return fgMode_; }
    double lastFrameGenGpuMs() const { return lastFrameGenGpuMs_; }
    void shutdown();

    // M13 control channel: live parameter update. cfg_.headGain is consumed
    // per frame in recordFrame (compose push constant), so the new value
    // applies to the NEXT processFrame - no engine re-init, no game
    // re-capture. Daemon-side only (single-threaded accept loop).
    void setHeadGain(float g) { cfg_.headGain = g; }
    // Blend and intensity are SEPARATE channels: headBlend is the blend
    // knob's slot, phase30.intensity feeds the compose pc.c.w mix. The
    // old dual-write let a blend push silently overwrite the intensity
    // knob (and vice versa) - callers that want the mix slot use
    // setIntensity/setPhase30 explicitly.
    void setHeadBlend(float b) { cfg_.headBlend = b; }
    void setSharpness(float s) { cfg_.sharpness = s; }
    float sharpness() const { return cfg_.sharpness; }

    // Phase 30 vendor parameter controls. setPhase30/setIntensity write
    // ONLY cfg_.phase30 - neither touches headBlend, so the sequence
    // setHeadBlend(b); setPhase30(p) cannot fight over a shared slot.
    void setPhase30(const Phase30Params& p) { cfg_.phase30 = p; }
    const Phase30Params& phase30() const { return cfg_.phase30; }
    void setIntensity(float v) { cfg_.phase30.intensity = v; }
    void setLocalTone(float v) { cfg_.phase30.localTone = v; }
    void setLocalStructure(float v) { cfg_.phase30.localStructure = v; }
    void setSkinStructure(float v) { cfg_.phase30.skinStructure = v; }
    void setAutoMask(bool v) { cfg_.phase30.autoMask = v; }
    void setStyleIndex(int v) { cfg_.phase30.styleIndex = v; }
    void setColorStrength(float v) { cfg_.phase30.colorStrength = v; }
    void setTemporal(bool v) { cfg_.temporal = v; }
    bool temporal() const { return cfg_.temporal; }
    // Drop temporal history: the NEXT frame runs the first-frame path
    // (histValid=0, plain compose). Use on stream switches (new game window,
    // a different photo in a batch) so stale history cannot bleed across
    // unrelated content. Also re-baselines the scene-cut thumb: the caller
    // already handled the transition, so the new content must not
    // double-fire the gate.
    void resetHistory() { framesDone_ = 0; resetAnchors(); cutThumbValid_ = false;
                          driftValid_ = false; histSuppressed_ = false;
                          driftCalm_ = driftSettle_ = 0; }

    uint32_t netW() const { return netW_; }
    uint32_t netH() const { return netH_; }
    float renderScale() const { return cfg_.renderScale; }
    const Stage* stages() const { return st_; }

    // Diagnostics (engine.cpp): one-shot device->host dump of an internal
    // buffer into `path`. which: 0=bufHist fp32[TOK*3], 1=oFeatV fp32[TOK*16],
    // 2=oHEAD fp32[TOK*4], 3=oX16 fp16[TOK*16], 4=bufStats u32[8],
    // 5=bufNetRgb fp32[netW*netH*3].
    bool debugFetch(int which, const char* path);

private:
    bool createFrontEnd();        // buffers/images/descriptors/transport pipelines
    void recordFrame(long frameIndex);  // into cmd (not submitted yet)
    // Emits the 71-block chain into cmd_: replays the cached secondary
    // chainCmd_, re-recording it first when chainDirty_.
    void recordChainCached();
    // framegen tail: timestamps + dispatch (on `pipe`) + readback copies
    // into cmd. rollPrev copies bufCurrFrame_ into bufPrevFrame_
    // (interpolate only).
    void recordFrameGenTail(const struct Push& p, VkPipeline pipe, bool rollPrev);
    // temporal section: temporal_stats (gain-anchor luma sums + slot
    // rotation, optional) then temporal_nr (history blend + head rewrite).
    // Shared by the scale==1.0 and scale<1.0 record paths; engine_temporal.cpp.
    void recordTemporal();
    bool submitAndReadback(void* out, size_t bytes,
                           std::chrono::steady_clock::time_point t0,
                           FrameStats* stats);
    // Drop FG anchors (mode switch, extent change, stream reset): the next
    // interpolateAnchors copies the newest processed anchor until a second
    // pair exists.
    void resetAnchors() { anchorCount_ = 0; havePrevProcessed_ = false; }
    // Scene-cut gate (engine.cpp): compares a strided 64x36 luma thumb of
    // bgraIn against the previous frame's. True = unrelated content; the
    // caller drops framesDone_ so the frame runs the first-frame path
    // (histValid=0) instead of blending stale history.
    bool detectSceneCut(const uint8_t* bgraIn);
    void recordDebugCopies();     // frame==1 chain-path readback into bufDbg
    void writeDebugDumps();       // map bufDbg, write live_*.{bin,f16}
    void barrierAll();

    EngineConfig cfg_{};
    uint32_t W_ = 0, H_ = 0;                 // canvas
    uint32_t regionW_ = 0, regionH_ = 0;
    uint32_t netW_ = 0, netH_ = 0;           // vendor-aligned extent (mult 64, min 320)
    Stage st_[7]{};
    uint64_t T6_ = 0;
    uint32_t T6P_ = 0;

    VkContext vkc_;
    WeightsStore ws_;
    ChainArena arena_;
    ChainRecorder rec_;
    ChainProf prof_;

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkCommandBuffer chainCmd_ = VK_NULL_HANDLE;   // secondary: cached recordChain stream
    bool chainDirty_ = true;                      // re-record chainCmd_ on next processFrame
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool tsPool_ = VK_NULL_HANDLE;
    float tsPeriodNs_ = 1.0f;

    VkDescriptorSetLayout dsLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet setFinal_ = VK_NULL_HANDLE;

    VkPipeline pipeDecode_ = VK_NULL_HANDLE, pipeFeatures_ = VK_NULL_HANDLE,
               pipeCompose_ = VK_NULL_HANDLE, pipeEncode_ = VK_NULL_HANDLE;
    VkPipeline pipeRescale_ = VK_NULL_HANDLE, pipeFrameGen_ = VK_NULL_HANDLE,
               pipeTemporalNr_ = VK_NULL_HANDLE, pipeFgInterp_ = VK_NULL_HANDLE,
               pipeStats_ = VK_NULL_HANDLE, pipeMotionEst_ = VK_NULL_HANDLE;
    VkBuffer bufNetRgb_ = VK_NULL_HANDLE, bufFinLow_ = VK_NULL_HANDLE;
    VkDeviceMemory memNetRgb_ = VK_NULL_HANDLE, memFinLow_ = VK_NULL_HANDLE;
    VkDescriptorSet setDown_ = VK_NULL_HANDLE, setUp_ = VK_NULL_HANDLE, setFrameGen_ = VK_NULL_HANDLE;

    VkDeviceMemory memImgIn_ = VK_NULL_HANDLE, memUpload_ = VK_NULL_HANDLE,
                   memRgb_ = VK_NULL_HANDLE, memMax_ = VK_NULL_HANDLE,
                   memT_ = VK_NULL_HANDLE, memFin_ = VK_NULL_HANDLE, memNr_ = VK_NULL_HANDLE,
                   memHist_ = VK_NULL_HANDLE, memRbFinal_ = VK_NULL_HANDLE,
                   memDbg_ = VK_NULL_HANDLE, memImgFinal_ = VK_NULL_HANDLE,
                   memLastP_ = VK_NULL_HANDLE, memFbStats_ = VK_NULL_HANDLE, memHp_ = VK_NULL_HANDLE,
                   memPrevFrame_ = VK_NULL_HANDLE, memCurrFrame_ = VK_NULL_HANDLE, memInterpOut_ = VK_NULL_HANDLE,
                   memPrevRaw_ = VK_NULL_HANDLE, memCurProc_ = VK_NULL_HANDLE,
                   memMidRaw_ = VK_NULL_HANDLE, memStats_ = VK_NULL_HANDLE,
                   memHistPrev_ = VK_NULL_HANDLE, memMv_ = VK_NULL_HANDLE,
                   memMvRb_ = VK_NULL_HANDLE;
    VkImage imgIn_ = VK_NULL_HANDLE, imgFinal_ = VK_NULL_HANDLE;
    VkImageView imgInView_ = VK_NULL_HANDLE, viewFinal_ = VK_NULL_HANDLE;
    VkBuffer bufUpload_ = VK_NULL_HANDLE, bufRgb_ = VK_NULL_HANDLE, bufMax_ = VK_NULL_HANDLE,
             bufT_ = VK_NULL_HANDLE, bufFin_ = VK_NULL_HANDLE, bufNr_ = VK_NULL_HANDLE,
             bufHist_ = VK_NULL_HANDLE, bufRbFinal_ = VK_NULL_HANDLE, bufDbg_ = VK_NULL_HANDLE,
             bufLastPresented_ = VK_NULL_HANDLE, bufFbStats_ = VK_NULL_HANDLE, bufHp_ = VK_NULL_HANDLE,
             bufPrevFrame_ = VK_NULL_HANDLE, bufCurrFrame_ = VK_NULL_HANDLE, bufInterpOut_ = VK_NULL_HANDLE,
             bufPrevRaw_ = VK_NULL_HANDLE, bufCurProc_ = VK_NULL_HANDLE,
             bufMidRaw_ = VK_NULL_HANDLE, bufStats_ = VK_NULL_HANDLE,
             bufHistPrev_ = VK_NULL_HANDLE, bufMv_ = VK_NULL_HANDLE,
             bufMvRb_ = VK_NULL_HANDLE;
    // D5C_DEBUG_MV: bufMv_ is staged into host-visible bufMvRb_ every
    // frame (record tail); every 30th completed frame logs median mv and
    // the nonzero-block share. mvRbPtr_ is the persistent map.
    void* mvRbPtr_ = nullptr;
    bool mvDebug_ = false;
    // D5C_MV_BETA / D5C_MV_INTERP runtime knobs, pushed to temporal_nr and
    // temporal_stats as pc.c[1] (unconditional base pull weight on moving
    // pixels) and pc.c[2] (1 = bilinear mv between block centres, 0 =
    // nearest block). v12: the every-frame pull re-anchors the carried
    // residual to the screen-fixed net grain when it dominates (the
    // crawling grain on roads/walls), but with the smoothed carry
    // (D5C_MV_RSM) a small base pull is pure damping - it converges
    // tap-selection noise without re-anchoring the field. Shipped
    // default 0.10; 0 = verbatim carry (measured 3x worse flow flicker
    // on the GTA IV flythrough), >=0.3 reproduces the old re-anchor.
    float mvBeta_ = 0.10f;
    float mvInterp_ = 1.0f;
    // D5C_MV_PRAD/PTH/PMARG: per-pixel carry-tap refinement in
    // temporal_nr/stats (pc.b[0..2]). When the block-mv warp leaves the
    // stored previous input > PTH/255 off the current pixel, the integer
    // taps within PRAD net px of the warp - plus always the screen cell -
    // are rescored on max-channel |col01 - P|; a tap better by > PMARG/255
    // relocates the carry (mv becomes p - tap). PRAD = 0 disables.
    float mvPrad_ = 2.0f;
    float mvPth_ = 2.0f;
    float mvPmarg_ = 1.0f;
    // D5C_MV_RSM: carried-residual smoothing radius in net px
    // (pc.b[3]). On moving pixels the residual source is the mean of
    // E - P over the (2*RSM+1)^2 taps around q instead of the single
    // warped tap: the flicker cost of a warp error is the spatial
    // gradient of the carried residual field, and block mvs miss by
    // 1-4 px on grain - a smooth carried field keeps small errors
    // cheap while staying content-attached. 0 = verbatim tap. Still
    // pixels always keep the verbatim (byte-exact) tap. Shipped 2
    // (5x5 neighbourhood): the best measured flow/static balance on
    // the GTA IV flythrough (0.048/0.074 vs 0.051/0.079 at radius 1).
    float mvRsm_ = 2.0f;
    // D5C_RF_ALO/AHI/RLO: moving-pixel refresh-pull bands (LSB units,
    // pc.d[1..3]). emit = col01 + rh + w*(rt - rh) with
    //   w = max(smoothstep(ALO,AHI,dF), smoothstep(RLO,2.5*RLO,dr),
    //           BETA*min(|mv|,1)):
    // honest warps carry the residual verbatim; a warp that cannot
    // explain the input (dF, post-refinement) or a residual the net no
    // longer agrees with (dr) blends to the fresh candidate instead.
    float rfAlo_ = 5.0f;
    float rfAhi_ = 14.0f;
    float rfRlo_ = 32.0f;
    // D5C_RF_SCAP: still-pixel refresh step cap in LSB (pc.a[3]). The
    // armed-budget refresh steps the held residual by
    // max(R_STEP_MIN, R_FRAC*|e|) per frame - visible 5-10 LSB jumps on
    // pixels parked next to motion whose residual went badly stale
    // (manifest pan fixture's still-band flicker). Capping the step at
    // ~1.5 LSB makes the convergence a sub-visible trickle inside the
    // same 16-frame budget. <=0 = uncapped (old behaviour).
    float rfScap_ = 1.5f;
    // D5C_MV_ZBIAS / D5C_MV_ZABS: motion_est zero-motion bias,
    // (0,0) when SAD00 <= SADmin*(1+ZBIAS) + 64*ZABS/255.
    // Pushed to motion_est as pc.c[1]/pc.c[2].
    // D5C_MV_PIXSTATIC: per-pixel static gate in LSB, shipped as a RAMP
    // (v12): the block mv is scaled by smoothstep(c3 .. 2.25*c3, dF0)
    // where dF0 is the pixel's unwarped input delta - a pixel whose
    // input did not change keeps the byte-exact still path even inside
    // a moving block, a clearly-moving pixel keeps the full warp, and
    // the boundary band carries proportionally. The ramp replaced a
    // hard force-to-zero: pixels whose input noise straddled the cutoff
    // alternated carried/still frame to frame and flickered at every
    // threshold value (manifest pan fixture ~10%/frame). Default 0.75
    // (ramp 0.75..1.7 LSB) keeps the flow-flicker win on the flythrough;
    // values >=2 anchor too many slow-drift pixels and degrade the
    // motion metric (0.75 -> 0.051/0.079, 2.0 -> 0.059/0.098 on the
    // hard pan slice). 0 = off. Pushed to temporal_nr/stats as pc.c[3].
    // D5C_MV_SUBT: sub-pixel threshold in net pixels; the motion_est
    // parabolic refine runs on every interior pick (including (0,0), so
    // bias-pinned blocks still report their sub-pixel drift), and a
    // final vector shorter than SUBT collapses back to exact (0,0) -
    // refined 1-2 LSB noise lands under ~0.15 px while real drift sits
    // higher. Pushed to motion_est as pc.c[3].
    // D5C_MV_EMA: per-block temporal EMA on the reported vector
    // (pc.c[0] of motion_est, weight of the new sample; 1.0 = raw).
    // Vectors >= 1 px bypass the EMA (the +-0.5-clamped sub-pixel refine
    // cannot reach 1 px, so real integer motion never lags). Default is
    // RAW: smoothing <1 regressed the _pan_seq static gate (alternation
    // at the 1px snap boundary + stale persistence after motion stops);
    // the IMPT evidence gate already rejects flat-region noise.
    // Kept as an opt-in knob.
    // D5C_MV_IMPT: confidence gate on the (0,0)-pick sub-pixel refine -
    // the predicted SAD improvement den*delta^2/2 summed over axes must
    // beat this (SAD units, luma in 0..1). Detailed content with real
    // drift passes; SAD-flat blocks stay (0,0). Pushed as pc.d[1].
    float mvZbias_ = 0.15f;
    float mvZabs_ = 2.0f;
    float mvPixStatic_ = 0.75f;
    float mvSubt_ = 0.10f;
    float mvEma_ = 1.0f;
    float mvImpt_ = 0.06f;
    double lastFrameGenGpuMs_ = 0.0;
    bool havePrevProcessed_ = false;   // set after a successful processFrame
    int fgMode_ = 1;                   // 1 = transfer (+0), 2 = anchor interp (+1)
    int anchorCount_ = 0;              // mode-2 anchors held (0..2); mode 1
                                       // mirrors havePrevProcessed_ (0/1)
    uint64_t framesDone_ = 0;          // frames completed on THIS instance;
                                       // 0 -> first-frame path (no history)

    // Scene-cut gate state (detectSceneCut): a strided 64x36 luma thumb of
    // the previous host input (~2.3 KB, plain member array - no GPU
    // buffers). A mean abs diff above cutLimit_ (normalized 0..1, default
    // 0.15 ~= 38/255 - the reference content-reset level) flags a cut.
    // Env overrides read in init(): D5C_CUT_LIMIT / DLSS5_CUT_LIMIT
    // (0 disables), D5C_DEBUG_CUT=1 logs each cut to stderr.
    static constexpr int kCutThumbW = 64, kCutThumbH = 36;
    uint8_t cutThumb_[kCutThumbW * kCutThumbH]{};
    bool cutThumbValid_ = false;       // first frame / after init or reset
    float cutLimit_ = 0.15f;
    bool cutDebug_ = false;

    // Motion gate for history participation (detectSceneCut also counts
    // thumb cells whose luma moved > kTmCellDelta). When the moved fraction
    // exceeds tmSuppressHi_ the frame runs histValid=0 - a pure prediction
    // equal to temporal-off: at the ~40 ms x86-daemon cadence a blended
    // stale delta visibly lags the content. Below tmSuppressLo_ the latch
    // re-engages - on near-static frames (menu + moving cursor) the
    // hold/verbatim emit kills the global tone re-derivation ("breathing")
    // that temporal-off exposes. The hysteresis band holds the last state
    // so a mid-motion scene can't flap the mode per frame. History keeps
    // refreshing on suppressed frames, so re-engagement is transient-free.
    // Envs: D5C_TMGATE / DLSS5_TMGATE / NR_LAYER_TMGATE =0 disables the
    // gate entirely; *_TMGATE_HI / *_TMGATE_LO retune the thresholds.
    static constexpr int kTmCellDelta = 10;   // /255 luma per cell = "moved"
    float tmFrac_ = 0.0f;                     // last frame's moved fraction
    bool histSuppressed_ = false;
    bool tmGate_ = true;
    float tmSuppressHi_ = 0.20f;
    float tmSuppressLo_ = 0.05f;
    // Displacement anchor on top of the instantaneous gate: a slow pan
    // (~0.25 px/frame) moves each cell <kTmCellDelta so tmFrac_ ~ 0 and a
    // per-frame velocity signal can never see it, yet over ~2-3 px of
    // travel the emit latch is still holding the frozen mask on content
    // that slid under it (field report: "scene holds ~1 s, then slowly
    // turns - mask visible, applied crooked"). driftThumb_ snapshots the
    // thumb at the last SETTLED pose; driftFrac_ is the fraction of cells
    // displaced >tmDriftDelta_ from that anchor on the 3x3-box-SMOOTHED
    // thumb - box filtering halves the noise sigma so codec grain never
    // reaches the displacement threshold (measured: sigma=6 grain -> 1%
    // displaced, a 1 px pan -> 10%, 2 px -> 19%), while any real
    // translation accumulates regardless of per-frame speed. driftFrac >
    // tmDriftHi_ suppresses history like the HI tripwire. While
    // suppressed the anchor stays FROZEN, so a continuing pan keeps
    // driftFrac rising and cannot plateau-release mid-motion: release
    // requires near-zero coarse velocity (tmFrac_ < tmDriftRelLo_ - even
    // a 0.25 px/frame pan still moves ~2% of coarse cells) AND driftFrac_
    // plateaued (|delta| < tmDriftEps_) for tmDriftCalmN_ straight
    // frames - content parked at a new pose - then the anchor re-bases
    // there. While live, driftFrac < tmDriftLo_ for tmDriftSettleN_
    // straight frames also re-bases (content settled near the anchor;
    // keeps the measure a displacement from a real pose rather than
    // unbounded history).
    // Envs: D5C_TMGATE_DRIFT (0 disables the drift path),
    // D5C_TMGATE_DRIFT_HI / _LO / _EPS / _CALM / _SETTLE / _DELTA.
    uint8_t driftThumb_[kCutThumbW * kCutThumbH]{};
    bool  driftValid_    = false;
    float driftFrac_     = 0.0f;
    float driftPrev_     = 0.0f;
    int   driftCalm_     = 0;
    int   driftSettle_   = 0;
    bool  tmDriftOn_     = true;
    int   tmDriftDelta_  = 5;       // smoothed-thumb luma delta = displaced
    float tmDriftHi_     = 0.20f;   // displaced-cell fraction = suppress
    float tmDriftLo_     = 0.05f;   // below this, anchor may re-base
    float tmDriftRelLo_  = 0.005f;  // near-zero coarse velocity = settled
    float tmDriftEps_    = 0.008f;  // |d(driftFrac)| plateau = settled
    int   tmDriftCalmN_  = 8;       // settled frames before release+rebase
    int   tmDriftSettleN_= 10;      // low-drift frames before live re-base
    bool tmDebug_ = false;
    void* uploadPtr_ = nullptr;
    void* rbFinalPtr_ = nullptr;             // persistently mapped readback

    // debug dump offsets inside bufDbg (frame==1)
    uint64_t dbgOffFeat_ = 0, dbgOffHead_ = 0;
};

} // namespace d5c
