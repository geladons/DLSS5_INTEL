// m12_hotkeys.h - owner hotkeys for the m12 dxgi proxy.
//
// CTRL+ALT+X: toggle processing pause - flips the same %TEMP%\m12_pause.flag
//             the processor polls every present (file stays as a second,
//             scriptable control channel).
// CTRL+ALT+Q: full detach - restores every vtable patched in place, releases
//             the processors and makes the proxy inert for the rest of the
//             process lifetime (game continues untouched).
// The poller thread starts on the first hooked factory call, i.e. outside
// the loader lock, in game context. GetAsyncKeyState state polling only -
// no message hooks, no injection, nothing for anti-tamper to fingerprint.
#pragma once

// Starts the hotkey thread once (idempotent).
void m12_hotkeys_ensure_started();
