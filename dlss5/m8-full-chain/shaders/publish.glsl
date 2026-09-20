/* The vendor's rounding points, shared by every shader that has to reproduce them.
 *
 * `precise` is not decoration here: the gate rounds to half between the multiply and
 * the add, and an FMA contraction would skip that rounding and give a different answer
 * from the reference.
 *
 * Byte-verbatim copy of reference/dlss-nr-on-intel/src/gpu/publish.glsl
 * (design doc m6b-graph-design.md SS A.1: "Reuse M3's verbatim publish.glsl").
 */
#ifndef PUBLISH_GLSL
#define PUBLISH_GLSL

/* The hardware float16 conversion, done as INTEGER bit manipulation.
 *
 * The reference spells this `unpackHalf2x16(packHalf2x16(vec2(x, 0.0))).x`, but on
 * the Arc driver that pair is folded back to a no-op (f32->f16->f32 "value
 * preserving") and every rounding point silently vanishes: out-of-f16-range
 * inputs (65520, 1e30, 1e-8) pass through unconverted. Doing the round-to-
 * nearest-even by hand on the bit pattern cannot be elided, and is bit-exact
 * against numpy's float16 over ordinary values, half subnormals and overflow
 * to infinity — the same semantics the F16C vcvtps2ph gives the CPU side.
 * (Caution: e4m3 relies on half_round flushing nothing; subnormals must round.)
 */

/* f32 bits -> f16 bits, round-to-nearest-even. Overflow -> inf, subnormals kept. */
uint f16_round_bits(uint bx) {
    uint sign = (bx >> 16) & 0x8000u;
    uint mant = bx & 0x7FFFFFu;
    int e = int((bx >> 23) & 0xFFu);
    if (e == 255) {                                   // inf / NaN
        if (mant == 0u) return sign | 0x7C00u;        // inf
        uint m16 = mant >> 13;                        // NaN: preserve payload
        return sign | 0x7C00u | (m16 != 0u ? m16 : 1u);  // (quiet only if it'd read as inf)
    }
    int hexp = e - 112;                               // rebase f32 exp (127) -> f16 (15)
    if (hexp >= 31) return sign | 0x7C00u;            // overflow -> inf (numpy semantics)
    if (hexp <= 0) {                                  // f16 subnormal (or zero)
        if (hexp < -10) return sign;                  // below half of min subnormal -> 0
        mant |= 0x800000u;
        int shift = 14 - hexp;
        uint m = mant >> shift;
        uint rem = mant & ((1u << shift) - 1u);
        uint halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (m & 1u) != 0u)) {
            m++;
            if (m == 0x400u) return sign | 0x400u;    // rounded up to 2^-14 min normal
        }
        return sign | m;
    }
    uint m = mant >> 13;
    uint rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1u) != 0u)) {
        m++;
        if (m == 0x400u) { m = 0u; hexp++; if (hexp >= 31) return sign | 0x7C00u; }
    }
    return sign | (uint(hexp) << 10) | m;
}

/* f16 bits -> widened f32 (exact; subnormal f16 expands to normal f32). */
float f16_widen(uint h) {
    uint sign = (h & 0x8000u) << 16;
    uint e = (h >> 10) & 0x1Fu;
    uint mant = h & 0x3FFu;
    uint b;
    if (e == 0u) {
        if (mant == 0u) b = sign;
        else {
            int exp = -1;
            while ((mant & 0x400u) == 0u) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            b = sign | (uint(114 + exp) << 23) | (mant << 13);
        }
    } else if (e == 31u) {
        b = sign | 0x7F800000u | (mant << 13);
    } else {
        b = sign | ((e - 15u + 127u) << 23) | (mant << 13);
    }
    return uintBitsToFloat(b);
}

float half_round(float x) { return f16_widen(f16_round_bits(floatBitsToUint(x))); }

float e4m3(float x) {
    float magnitude = min(abs(x), 448.0);
    int exponent = (floatBitsToInt(magnitude) >> 23) & 0xFF;
    exponent = max(exponent, 121) - 3;                 // 121-3 = the 2^-9 subnormal step
    float step = intBitsToFloat(exponent << 23);
    float reciprocal = intBitsToFloat((254 - exponent) << 23);
    float rounded = roundEven(magnitude * reciprocal) * step;
    return x < 0.0 ? -rounded : rounded;
}

float gate_activation(float x) {
    precise float wide = half_round(x);
    precise float clamped = clamp(wide, -4.0, 4.0);
    precise float linear = abs(clamped) * -0.055908203125;
    linear += 0.447265625;
    linear = half_round(linear);
    linear *= clamped;
    linear += 0.89453125;
    linear = half_round(linear);
    return half_round(wide * linear);
}

/* The publish an epilogue applies: bits 8-11 of a pass's `flags` pick the transform. */
float publish(uint epilogue, float value) {
    if (epilogue == 2u || epilogue == 3u) value = gate_activation(value);
    if (epilogue == 1u || epilogue == 3u) value = e4m3(value);
    if (epilogue == 4u) value = half_round(value);
    return value;
}

#endif
