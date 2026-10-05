# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.overlay_input - OverlayInputGuard: the overlay-visible input lane.
#
# Games on DInput/raw-input paths (GTA IV being the field case) pin the OS
# cursor three ways at once while playing:
#   - ClipCursor(rect) confines it system-wide - a rect smaller than the
#     client (a point-clip pins it completely) can leave the band region
#     unreachable;
#   - SetCapture redirects every mouse message - and WM_SETCURSOR - to the
#     game window even when the pointer sits over our band;
#   - ShowCursor(false) / SetCursor(NULL) leave whatever cursor exists
#     invisible.
# Every one of those has a LEGAL cross-process counter:
#   - ClipCursor(NULL) releases the SYSTEM-wide clip (it is not
#     thread-affine - any process may release another's clip);
#   - ReleaseCapture() drops capture held by any thread;
#   - ShowCursor(true) raises the shared display counter and SetCursor
#     repaints a real arrow while the pointer is over our band.
# The catch: the game re-arms its clip every frame, so the one-shot
# release at show() is not enough. This guard re-releases on a gentle
# watchdog (~180 ms) but ONLY while the overlay is visible, and skips a
# pass while a button is held over the panel (a mid-gesture
# ReleaseCapture kills Tk slider drags). On disarm() nothing stays
# modified - the guard never SETS a clip, so the game just re-arms its
# own on the next frame.
#
# Two modes on the same watchdog:
#   external window (real toplevel): the full pass - a real, visible OS
#     cursor must exist over the band so clicks land on Tk widgets.
#   in-layer (shm-composited): the layer draws the cursor sprite and eats
#     WM_SETCURSOR itself; the guard only releases FOREIGN confinement
#     (pin_only): a clip that does not cover the game client is dropped,
#     while a client-covering clip - including the layer's own deliberate
#     fullscreen ClipCursor - is left armed so the two never fight.
# ============================================================================
import tkinter as tk

from .overlay_win32 import _u32, _free_cursor, cursor_clip
from . import overlay_log

VK_LBUTTON = 0x01


class OverlayInputGuard:
    """Visibility-scoped cursor-release watchdog for a ControlOverlay.

    arm() runs on every show(), disarm() on every hide(); the watchdog
    self-terminates the moment the overlay reports hidden, so a stale
    timer can never keep freeing the game's input state in the
    background. The overlay instance is the back-reference (same pattern
    as InLayerUI): win provides after(), _is_shown() the gate, and the
    panel drag flags feed busy().
    """

    WATCH_MS = 180           # gentle re-release cadence (100-250 ms spec)
    _LOG_EVERY = 28          # ~5 s: a persistent re-clip logs sparsely

    def __init__(self, overlay):
        self.ov = overlay
        self._id = None          # pending after() handle
        self._game = 0           # game HWND (AttachThreadInput target)
        self._show_cursor = True
        self._pin_only = False
        self._passes = 0         # release passes this arm session
        self._found = 0          # passes that found an armed clip

    # ------------------------------------------------------------- API ---
    def arm(self, game_hwnd, show_cursor=True, pin_only=False):
        """Start the guard for a shown overlay; one immediate pass frees
        the cursor NOW so the first frames already have a usable pointer.
        show_cursor=False is the in-layer mode: only confinement work -
        the composited sprite is the cursor there and the game keeps
        owning the OS one (a ShowCursor raise on the game queue would
        draw a second cursor over the frame). pin_only=True scopes the
        clip release to foreign pins: the layer's own fullscreen
        ClipCursor covers the client and must NOT be released - the
        edge-tracked layer never re-arms a clip the guard stole."""
        self.disarm()
        try:
            self._game = int(game_hwnd or 0)
        except (TypeError, ValueError):
            self._game = 0
        self._show_cursor = bool(show_cursor)
        self._pin_only = bool(pin_only)
        self._passes = self._found = 0
        self._release("arm")
        self._log("[input] guard armed game=0x%x show_cursor=%d "
                  "pin_only=%d" % (
                      self._game, self._show_cursor, self._pin_only))
        self._schedule()

    def disarm(self):
        """hide()/teardown: stop the watchdog. Nothing needs restoring -
        the guard never SETS a clip, so once it stops re-releasing the
        game simply re-arms its own confinement on the next frame."""
        if self._id is not None:
            try:
                self.ov.win.after_cancel(self._id)
            except Exception:
                pass
            self._id = None
        if self._game or self._passes:
            self._log("[input] guard disarmed (%d passes, %d armed game "
                      "clips released)" % (self._passes, self._found))
        self._game = 0

    def busy(self):
        """A pointer gesture is live over the panel (button held, Tk or
        Phase30 slider drag): ReleaseCapture mid-gesture would kill the
        drag, so watchdog passes skip while this is True. Fail-safe side
        is 'busy': an unknown state must not disturb a gesture."""
        ov = self.ov
        try:
            if getattr(ov, "_dragging", False):
                return True
            p30 = getattr(ov, "p30_panel", None)
            if p30 is not None and p30.is_dragging():
                return True
            return bool(_u32.GetAsyncKeyState(VK_LBUTTON) & 0x8000)
        except Exception:
            return True

    # -------------------------------------------------------- internals --
    def _log(self, msg):
        overlay_log.log(msg)
        try:
            self.ov.on_log(msg)
        except Exception:
            pass

    def _schedule(self):
        if self._id is not None:
            return
        try:
            self._id = self.ov.win.after(self.WATCH_MS, self._tick)
        except tk.TclError:
            self._id = None

    def _tick(self):
        """Watchdog tick: runs ONLY while the overlay is visible - a
        hidden overlay leaves the game's input state strictly alone."""
        self._id = None
        try:
            shown = self.ov._is_shown()
        except tk.TclError:
            return               # toplevel gone - stay disarmed
        except Exception:
            shown = False
        if not shown:
            self._game = 0
            return
        if not self.busy():
            self._release("watchdog")
        self._schedule()

    def _release(self, why):
        """One release pass. The clip sample is taken first purely for
        the log: 'found an armed clip' tells a field report the game IS
        re-clipping under the overlay (vs a clip-free game where every
        pass is a no-op)."""
        clip = cursor_clip()
        _free_cursor(self._game or None, show=self._show_cursor,
                     pin_only=self._pin_only)
        self._passes += 1
        if clip is not None:
            self._found += 1
            if self._found <= 3 or self._passes % self._LOG_EVERY == 0:
                self._log("[input] %s: armed clip %s released (pass %d)"
                          % (why, clip, self._passes))
