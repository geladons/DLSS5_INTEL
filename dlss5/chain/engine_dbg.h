// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// dlss5/chain - internal debug-readback layout shared by the front-end
// (bufDbg_ sizing in engine_frontend.cpp) and the debug cluster
// (recordDebugCopies/writeDebugDumps in engine_debug.cpp). Not installed;
// TU-internal to the chain module.
// ============================================================================
#pragma once

#include "engine.h"

namespace d5c {

// --------------------------- debug readback layout (frame==1) -------------
struct DbgLayout {
    uint64_t szFeat, szHead, szX16, szADA, szMRG, szG2, szHW, szB4, szB22,
             szB38, szB48, szB69, szSin, szCos, szFRS;
    uint64_t offFeat, offHead, offX16, offADA, offMRG, offG2, offHW, offB4,
             offB22, offB38, offB48, offB69, offSin, offCos, offFRS;
    uint64_t total;
};

inline DbgLayout dbgLayout(const Stage st[7], uint32_t T6P) {
    auto al = [](uint64_t v) { return (v + 255) & ~255ull; };
    DbgLayout d{};
    d.szFeat = al((uint64_t)st[0].TOK * 16 * 4);
    d.szHead = al((uint64_t)st[0].TOK * 4 * 4);   // oHEAD is compact [TOK,4] now
    d.szX16 = al((uint64_t)st[0].TOK * 16 * 2);
    d.szADA = al((uint64_t)st[0].TOK * 32 * 4);
    d.szMRG = al((uint64_t)st[0].TOK * 32 * 4);
    d.szG2 = al((uint64_t)st[0].TOK * 32 * 4);
    d.szHW = al(1024);
    d.szB4 = al((uint64_t)st[2].TOK * 64 * 2);
    d.szB22 = al((uint64_t)st[5].TOK * 512 * 2);
    d.szB38 = al((uint64_t)T6P * 1024 * 2);
    d.szB48 = al((uint64_t)st[4].TOK * 256 * 2);
    d.szB69 = al((uint64_t)st[1].TOK * 32 * 2);
    d.szSin = al(32 * 4);
    d.szCos = al(32 * 4);
    d.szFRS = al((uint64_t)st[0].TOK * 32 * 2);
    d.offFeat = 0;
    d.offHead = d.offFeat + d.szFeat;
    d.offX16 = d.offHead + d.szHead;
    d.offADA = d.offX16 + d.szX16;
    d.offMRG = d.offADA + d.szADA;
    d.offG2 = d.offMRG + d.szMRG;
    d.offHW = d.offG2 + d.szG2;
    d.offB4 = d.offHW + d.szHW;
    d.offB22 = d.offB4 + d.szB4;
    d.offB38 = d.offB22 + d.szB22;
    d.offB48 = d.offB38 + d.szB38;
    d.offB69 = d.offB48 + d.szB48;
    d.offSin = d.offB69 + d.szB69;
    d.offCos = d.offSin + d.szSin;
    d.offFRS = d.offCos + d.szCos;
    d.total = d.offFRS + d.szFRS;
    return d;
}

} // namespace d5c
