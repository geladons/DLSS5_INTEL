#!/usr/bin/env python3
# ============================================================================
# pad.py - virtual X360 gamepad driver for the autotest rig.
#
# GTA IV ignores every injected keyboard/mouse path (DInput/raw input,
# SendInput, PostMessage, UIA - verified 2026-10-03). A ViGEmBus virtual
# X360 pad (Python vgamepad) is the ONLY synthetic input the game accepts:
# field report 2026-10-05 drove GTA IV menus AND gameplay end-to-end on
# this host ('Xbox 360 controller connected' toast, menu nav, on-foot
# movement + camera). Proven API usage pattern: Temp/_pad_btn.py,
# Temp/_pad_test.py, Temp/_drive_record.py.
#
# Usage model: one Pad.attach() per session, then either direct calls
# (press / hold_stick / reset) or a timed play_sequence(events):
#
#   events = list of (at_seconds, action) - at_seconds is relative to
#   sequence start; actions are small tuples:
#     ("press", name[, n[, gap]])     n taps of a named button
#     ("hold", lx, ly, rx, ry, secs)  set both sticks, wait secs
#     ("reset",)                      all inputs neutral
#
# Design notes for GTA IV:
#   - Stick state PERSISTS between events: a "hold" sets all four axes and
#     leaves them; the next hold re-sets all four. Continuous motion across
#     event gaps is intentional (gaps read as kept walking, not teleporting
#     back to neutral). The pad is always reset to neutral when the
#     sequence ends or the stop event fires.
#   - A pad attach failure is NOT fatal: attach() logs and returns False,
#     the caller skips driving and still captures. play_sequence() on an
#     unattached pad is a logged no-op.
#   - Menu navigation is timed, not pixel-gated: the main menu arrives
#     ~110-200 s after the game window (varies with daemon load), so the
#     menu_play sequence SWEEPS presses across that whole window. The
#     sweep is idempotent by construction - every button used is harmless
#     on every screen it can land on (see _menu_play_events comments).
# ============================================================================
import threading
import time

# X360 axis range -32768..32767. Magnitudes below are the field-proven
# values from Temp/_drive_record.py (walk bursts + figure-eight camera).
WALK_FWD = 22000        # left stick +y = push forward = walk
PAN_R = 16000           # right stick +x = camera right
PAN_L = -14000          # right stick -x = camera left
PAN_UP = 4000           # slight camera rise while panning

PRESS_HOLD_S = 0.12     # button down time per tap (from _pad_btn.py)
PRESS_GAP_S = 0.45      # pause after a tap (from _pad_btn.py)


class Pad(object):
    """One ViGEm X360 pad. Lazy import: a host without vgamepad/ViGEmBus
    degrades to 'no drive', never a crash."""

    def __init__(self, log=None):
        self.log = log or (lambda m: print(m, flush=True))
        self.vg = None
        self.gp = None
        self.btn = {}
        self.t0 = None

    # ---- lifecycle --------------------------------------------------------
    def attach(self):
        """Create the virtual pad. Returns False (already logged) when
        vgamepad or the ViGEmBus driver is unavailable."""
        try:
            import vgamepad as vg
            self.vg = vg
            self.gp = vg.VX360Gamepad()
        except Exception as e:
            self.log("pad attach failed: %r - drive disabled" % e)
            self.gp = None
            return False
        B = vg.XUSB_BUTTON
        self.btn = {
            "A": B.XUSB_GAMEPAD_A,
            "B": B.XUSB_GAMEPAD_B,
            "X": B.XUSB_GAMEPAD_X,
            "Y": B.XUSB_GAMEPAD_Y,
            "START": B.XUSB_GAMEPAD_START,
            "BACK": B.XUSB_GAMEPAD_BACK,
            "UP": B.XUSB_GAMEPAD_DPAD_UP,
            "DOWN": B.XUSB_GAMEPAD_DPAD_DOWN,
            "LEFT": B.XUSB_GAMEPAD_DPAD_LEFT,
            "RIGHT": B.XUSB_GAMEPAD_DPAD_RIGHT,
            "LB": B.XUSB_GAMEPAD_LEFT_SHOULDER,
            "RB": B.XUSB_GAMEPAD_RIGHT_SHOULDER,
        }
        self.log("pad attached (ViGEm X360)")
        return True

    def release(self):
        """Neutralize all inputs. The pad object itself is GC-released by
        vgamepad; there is no explicit disconnect call."""
        try:
            self.reset()
        except Exception:
            pass

    # ---- primitives -------------------------------------------------------
    def press(self, name, n=1, gap=PRESS_GAP_S):
        """n taps of a named button (0.12 s down each, gap between)."""
        b = self.btn.get(name)
        if b is None:
            self.log("unknown button %r - skipped" % name)
            return
        for _ in range(n):
            self.gp.press_button(button=b)
            self.gp.update()
            time.sleep(PRESS_HOLD_S)
            self.gp.release_button(button=b)
            self.gp.update()
            time.sleep(gap)

    def hold_stick(self, lx, ly, rx, ry, secs, stop=None):
        """Set both sticks to the given axes and wait secs. State is left
        in place afterwards - callers chain holds for continuous motion."""
        self.gp.left_joystick(x_value=lx, y_value=ly)
        self.gp.right_joystick(x_value=rx, y_value=ry)
        self.gp.update()
        self._sleep(secs, stop)

    def reset(self):
        """All buttons up, both sticks neutral."""
        if self.gp is not None:
            self.gp.reset()
            self.gp.update()

    def _sleep(self, secs, stop):
        """Interruptible wait (0.1 s granularity on a stop event)."""
        end = time.time() + secs
        while time.time() < end:
            if stop is not None and stop.is_set():
                return
            time.sleep(min(0.1, end - time.time()))

    def _elapsed(self):
        return time.time() - self.t0 if self.t0 else 0.0

    # ---- sequencer --------------------------------------------------------
    def play_sequence(self, events, stop=None):
        """Run a timed event list. events = [(at_seconds, action), ...];
        late events still fire (catch-up) - a slow menu must not lose
        presses. Returns early when the stop event is set. Always leaves
        the pad neutral."""
        if self.gp is None:
            self.log("no pad attached - sequence skipped")
            return
        self.t0 = time.time()
        self.log("sequence start: %d events" % len(events))
        for at, act in sorted(events, key=lambda e: e[0]):
            while True:
                rem = self.t0 + at - time.time()
                if rem <= 0:
                    break
                if stop is not None and stop.is_set():
                    self.log("sequence stopped at t=%.1f" % self._elapsed())
                    self._neutral()
                    return
                time.sleep(min(0.2, rem))
            try:
                self._dispatch(act, stop)
            except Exception as e:
                self.log("action %r failed: %r" % (act, e))
        self.log("sequence done at t=%.1f" % self._elapsed())
        self._neutral()

    def _neutral(self):
        try:
            self.reset()
        except Exception:
            pass

    def _dispatch(self, act, stop):
        kind = act[0]
        if kind == "press":
            name = act[1]
            n = act[2] if len(act) > 2 else 1
            gap = act[3] if len(act) > 3 else PRESS_GAP_S
            self.log("t=%.1f press %s x%d" % (self._elapsed(), name, n))
            self.press(name, n, gap)
        elif kind == "hold":
            _, lx, ly, rx, ry, secs = act
            self.log("t=%.1f hold sticks l=(%d,%d) r=(%d,%d) %.1fs"
                     % (self._elapsed(), lx, ly, rx, ry, secs))
            self.hold_stick(lx, ly, rx, ry, secs, stop)
        elif kind == "reset":
            self.log("t=%.1f reset" % self._elapsed())
            self.reset()
        else:
            self.log("t=%.1f unknown action %r" % (self._elapsed(), act))


# ============================================================================
# Sequences - data tables built by the helpers below. Timings are SECONDS
# RELATIVE TO SEQUENCE START, which capture_session fires right after the
# game window appears (~+21 s post-launch on this host). The main menu
# lands ~+150 s post-launch under layer+daemon -> ~t=125-130 here, but the
# sweep windows are deliberately wide because the load varies.
# ============================================================================

def _wander_events(t_start, horizon, a_every=1):
    """Gameplay motion loop repeated to `horizon`: walk-forward bursts with
    camera pans (the field-proven figure-eight-ish pattern), an A sprint
    tap every `a_every` cycles (a_every=0 disables)."""
    ev = []
    t = float(t_start)
    cyc = 0
    while t < horizon:
        ev.append((t + 0.0, ("hold", 0, WALK_FWD, 0, 0, 2.6)))          # walk
        ev.append((t + 3.1, ("hold", 0, WALK_FWD, PAN_R, 0, 2.4)))      # walk+panR
        ev.append((t + 6.0, ("hold", 0, 0, PAN_L, PAN_UP, 2.0)))        # sweepL
        if a_every and cyc % a_every == 0:
            ev.append((t + 8.2, ("press", "A")))                      # sprint tap
        ev.append((t + 8.7, ("hold", 0, WALK_FWD, 0, 0, 2.2)))          # walk
        t += 11.5
        cyc += 1
    return ev


def _menu_play_events(horizon=2400.0):
    """Legal/intro -> game-select -> main menu PLAY -> in-world wander.

    Phase 1 (t=8..80): START taps dismiss R*/legal/intro screens while the
    game loads under the layer (~2x slower than native). They are no-ops
    everywhere else they can land this early.

    Phase 2 (t=85..225): the menu-arrival sweep - A ONLY. A confirms on
    every screen it can land on (game-select -> GTA IV default; main menu
    -> PLAY cursor default), is ignored on loading screens, and is a
    harmless sprint tap once in-world. START is deliberately ABSENT from
    here on: START on live gameplay opens the PAUSE MAP and nothing in a
    blind sequence can reliably close it (B = 'back' did not resume in the
    2026-10-07 run - a whole record window was lost to map panning, and a
    second START just toggles). The only STARTs are the phase-1 ones at
    t<=80, which cannot reach gameplay: in-world cannot happen before
    ~t=125 (menu ~110 earliest + PLAY + ~15 s save load). If a late legal
    screen or a 'Press Start' gate outlives t=80 the sequence stalls on
    that screen - a bounded, diagnosable failure, never a poisoned
    gameplay capture. Measured (2026-10-07): menu up ~t=125-130, PLAY hit
    ~t=140, in-world ~t=150 (Hove Beach apartment, 'Load Successful').

    Phase 2.5 (t=215..232): transition. RIGHT-stick-only pans + A presses:
    the right stick pans the camera in gameplay but is ignored by menus,
    and the extra A's extend the menu sweep tail. If the menu is somehow
    still up this late the presses keep working; if the save already
    loaded, the camera visibly moves - either way the capture wins.

    Phase 3 (t>=235): the save is loaded - full wander loop (walk bursts +
    camera) for real gameplay footage. Left stick is always pushed UP:
    that is 'walk forward' in-world, and in the pathological case that a
    menu is still open it only scrolls the cursor to the TOP item - which
    is PLAY anyway, so even a stuck menu self-recovers on the next A.
    Note the pad state PERSISTS across events, so a stall inside vgamepad
    (observed: multi-second blocks under game load) leaves the last
    commanded stick held - the character keeps walking, the catch-up
    burst later is harmless. Timing is therefore stall-tolerant.
    """
    ev = []
    for t in (8, 20, 32, 44, 56, 68, 80):
        ev.append((float(t), ("press", "START")))
    for t in range(85, 215, 10):
        ev.append((float(t), ("press", "A")))
    # transition: right-stick pans (menu-inert) + straggler A presses
    ev += [
        (215.0, ("hold", 0, 0, PAN_R, 0, 2.5)),
        (218.5, ("press", "A")),
        (219.5, ("hold", 0, 0, PAN_L, PAN_UP, 2.5)),
        (223.0, ("press", "A")),
        (224.0, ("hold", 0, 0, -8000, 0, 2.5)),
        (227.5, ("press", "A")),
        (229.0, ("hold", 0, 0, PAN_R, 0, 2.5)),
        (232.5, ("press", "A")),
        (233.0, ("hold", 0, 0, 0, 0, 1.5)),
    ]
    ev += _wander_events(235.0, horizon, a_every=2)
    return sorted(ev, key=lambda e: e[0])


SEQUENCES = {
    "none": [],
    "menu_play": _menu_play_events(),
    "wander": _wander_events(6.0, 2400.0, a_every=3),
}


if __name__ == "__main__":
    # standalone smoke drive: python pad.py [menu_play|wander] [horizon_s]
    import sys
    seq = sys.argv[1] if len(sys.argv) > 1 else "wander"
    horizon = float(sys.argv[2]) if len(sys.argv) > 2 else 2400.0
    evs = [e for e in SEQUENCES[seq] if e[0] <= horizon]
    p = Pad()
    if p.attach():
        p.play_sequence(evs, stop=threading.Event())
