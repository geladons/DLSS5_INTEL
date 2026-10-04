// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m11d - death witnesses: stamp the failure class into the log before the
// process dies (see crash_witness.cpp).
// ============================================================================
#pragma once

// Installs process-wide handlers for std::terminate, pure-virtual calls,
// invalid CRT parameters, abort(), and an unhandled-SEH filter. Call once
// at the top of main.
void install_death_witnesses();
