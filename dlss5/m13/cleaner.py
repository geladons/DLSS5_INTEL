# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.cleaner - "Remove all traces": one-button clean slate.
#
# Two halves, mirroring deploy.py:
#   TraceScanner - pure inventory, never deletes (preview + post-verify)
#   Cleaner      - removes what the scanner found, restores backups
#
# What counts as a trace:
#   game dirs    dlls byte-identical to a production source (sha1 - including
#                foreign-NAMED copies), *.m13bak backups; scanned in every
#                configured game exe dir AND its dual-renderer sister
#                (bin\x64 <-> bin\x64_dx12). A candidate-named dll that is
#                neither ours nor parked over our backup is FOREIGN:
#                reported, never touched.
#   registry     HKCU Vulkan ImplicitLayers values pointing at OUR manifests
#   daemon       a running m11d.exe (stopped via processes.stop_pid - there
#                is no NRCT quit command; TerminateProcess is the manager's
#                normal stop path, same as ManagedProcess.stop)
#   temp files   every flag/knob/log path the stack writes (exact paths +
#                glob patterns; see default_temp_paths)
#   config dir   %LOCALAPPDATA%\DLSS5Manager - RENAMED (not rmtree'd, the
#                app recreates it) and only when include_config=True
#
# Every step logs "[clean] <step> ok=<0|1> path=... <detail>" through the
# log callable - same structured style as launcher's [launch] lines.
# ============================================================================
import glob
import os
import time
from dataclasses import dataclass

from . import deploy
from . import gamelaunch
from . import paths
from . import phase30
from . import processes as _procs
from . import screenmode
from .config import CONFIG_DIR as _DEFAULT_CONFIG_DIR
from .deploy import BACKUP_SUFFIX, Deployer, DeployError, LayerRegistry

DAEMON_EXE = "m11d.exe"
CHAIN_DLL = "d5c_chain.dll"

# Candidate dll names: everything any mode deploys plus the chain engine.
KNOWN_DLL_NAMES = frozenset(
    name for names in deploy.MODE_DLLS.values() for name in names
) | {CHAIN_DLL}

# Manifest basenames that identify OUR Vulkan layer registrations even when
# the registered path points at a moved/deleted copy (old bundle location).
_OUR_MANIFEST_NAMES = frozenset(
    os.path.basename(p).lower()
    for p in (deploy.MANIFEST_X64, deploy.MANIFEST_X86))

_DAEMON_STOP_TIMEOUT_S = 5.0


@dataclass
class Trace:
    kind: str        # 'game_dll' | 'game_backup' | 'foreign' | 'registry' |
                     # 'daemon' | 'temp_file' | 'config_dir'
    path: str        # display path (file, registry value name, exe name)
    detail: str = "" # extra context (matched source, pid, target name)


# ---------------------------------------------------------- source index ---
def default_source_dirs():
    """Every dir that holds production dlls we may have deployed."""
    dirs = [
        paths.find("dxvk_x64"), paths.find("dxvk_x32"),
        os.path.dirname(deploy.M12_PROXY),          # m12 proxy + d5c_chain
        os.path.dirname(paths.find("m11d_exe")),    # daemon dir holds chain
        os.path.dirname(deploy.MANIFEST_X64),       # x64 layer dir holds chain
    ]
    out = []
    for d in dirs:
        if d and d not in out:
            out.append(d)
    return out


class SourceIndex:
    """sha1 map of every dll we ship, built from the deploy source dirs."""

    def __init__(self, dirs=()):
        self._by_hash = {}      # sha1 -> source path
        self._by_name = {}      # dll name (lower) -> [source paths]
        for d in dirs:
            self.add_dir(d)

    def add_dir(self, d):
        if not d or not os.path.isdir(d):
            return
        try:
            names = os.listdir(d)
        except OSError:
            return
        for name in names:
            p = os.path.join(d, name)
            if not name.lower().endswith(".dll") or not os.path.isfile(p):
                continue
            try:
                h = deploy._sha1(p)
            except OSError:
                continue
            self._by_hash.setdefault(h, p)
            self._by_name.setdefault(name.lower(), []).append(p)

    def match(self, path):
        """-> the source path whose bytes equal `path`, or None."""
        try:
            h = deploy._sha1(path)
        except OSError:
            return None
        return self._by_hash.get(h)

    def sources_for(self, dll_name):
        return list(self._by_name.get(dll_name.lower(), ()))


# --------------------------------------------------------------- helpers ---
def sister_dirs(game_dir):
    """[game_dir] + the dual-renderer sibling (x64 <-> x64_dx12) if it exists.
    Deploy never syncs sisters but old builds did - clean both."""
    dirs = [game_dir]
    norm = os.path.normpath(game_dir)
    base = os.path.basename(norm).lower()
    parent = os.path.dirname(norm)
    if base.endswith("_dx12"):
        sib = os.path.join(parent, base[:-len("_dx12")])
    else:
        sib = os.path.join(parent, base + "_dx12")
    if os.path.normcase(sib) != os.path.normcase(norm) and os.path.isdir(sib):
        dirs.append(sib)
    return dirs


def _cfg_dir(cfg):
    p = getattr(cfg, "path", None)
    if p:
        return os.path.dirname(os.path.abspath(p))
    return _DEFAULT_CONFIG_DIR


def default_temp_paths():
    """Exact paths + glob patterns for every flag/knob/log the stack writes.
    scan() expands both (glob.glob returns a plain path if it exists)."""
    t = os.environ.get("TEMP") or "."
    out = [
        gamelaunch.DX9_TRIGGER, gamelaunch.M12_PAUSE,
        gamelaunch.FREEZE_FLAG, gamelaunch.REPROC_FLAG,
        phase30.PHASE30_KNOB_FILE, phase30.PHASE30_KNOB_FILE + ".tmp",
        screenmode.KNOB_FILE,
        os.path.join(t, "m13_hudmask.cfg"),     # legacy stage-24 leftover
        os.path.join(t, "m13_*.flag"),          # any stray runtime flag
        os.path.join(t, "m13elev_*"),           # elevated-deploy leftovers
        os.path.join(t, "nr_layer_win*.log"),   # m11 layer (x64 + x86)
        os.path.join(t, "m12*.log"),            # m12_dxgi.log + m12_boot.log
    ]
    logs = paths.find("logs")                   # <dlss5|bundle>\logs
    if logs:
        out += [os.path.join(logs, "m11d.log"),
                os.path.join(logs, "m8b-live.log"),
                os.path.join(logs, "manager_*.log")]
    return out


class _WinregRegistry:
    """HKCU ImplicitLayers probe/remove through deploy.LayerRegistry.
    Wrapped so tests (and the cleaner) can inject a fake."""

    def registered_paths(self):
        return LayerRegistry.registered_paths()

    def unregister(self, value_name):
        LayerRegistry.unregister(value_name)

    def is_registered(self, value_name):
        return LayerRegistry.registered(value_name)


def count_summary(traces):
    """-> {kind: n} for the confirm dialog and tests."""
    out = {}
    for t in traces:
        out[t.kind] = out.get(t.kind, 0) + 1
    return out


# ---------------------------------------------------------------- scanner --
class TraceScanner:
    """Pure inventory: never deletes. Used to preview and to verify after."""

    def __init__(self, cfg, deploy_source_dir=None, temp_paths=None,
                 config_dir=None, registry=None, procs=None):
        self.cfg = cfg
        if deploy_source_dir is None:
            dirs = default_source_dirs()
        elif isinstance(deploy_source_dir, str):
            dirs = [deploy_source_dir]
        else:
            dirs = list(deploy_source_dir)
        self.sources = SourceIndex(dirs)
        self.temp_paths = (list(temp_paths) if temp_paths is not None
                           else default_temp_paths())
        self.config_dir = config_dir or _cfg_dir(cfg)
        self.registry = registry or _WinregRegistry()
        self.procs = procs or _procs

    def scan(self):
        traces = []
        traces += self._scan_games()
        traces += self._scan_registry()
        traces += self._scan_daemon()
        traces += self._scan_temp()
        if self.config_dir and os.path.isdir(self.config_dir):
            traces.append(Trace("config_dir", self.config_dir,
                                "kept unless removal is requested"))
        return traces

    # ------------------------------------------------------------ games ---
    def _game_dirs(self):
        out = []
        seen = set()
        for exe in (self.cfg.get("games") or {}):
            for d in sister_dirs(os.path.dirname(exe)):
                key = os.path.normcase(os.path.abspath(d))
                if key not in seen and os.path.isdir(d):
                    seen.add(key)
                    out.append(d)
        return out

    def _scan_games(self):
        traces = []
        for d in self._game_dirs():
            try:
                names = os.listdir(d)
            except OSError:
                continue
            for name in names:
                p = os.path.join(d, name)
                if not os.path.isfile(p):
                    continue
                low = name.lower()
                if low.endswith(BACKUP_SUFFIX):
                    traces.append(Trace(
                        "game_backup", p,
                        "backup of " + name[:-len(BACKUP_SUFFIX)]))
                    continue
                if not low.endswith(".dll"):
                    continue
                src = self.sources.match(p)
                if src:
                    det = ("foreign name, our bytes (%s)"
                           % os.path.basename(src)
                           if low not in KNOWN_DLL_NAMES
                           else "matches " + os.path.basename(src))
                    traces.append(Trace("game_dll", p, det))
                elif low in KNOWN_DLL_NAMES:
                    if os.path.exists(p + BACKUP_SUFFIX):
                        traces.append(Trace("game_dll", p,
                                            "foreign dll over our backup"))
                    else:
                        traces.append(Trace(
                            "foreign", p,
                            "not ours, no backup - left in place"))
        return traces

    # --------------------------------------------------------- registry ---
    def _scan_registry(self):
        own = set(paths.find_all("layer_x64") + paths.find_all("layer_x86"))
        traces = []
        for name in self.registry.registered_paths():
            if name in own or \
                    os.path.basename(name).lower() in _OUR_MANIFEST_NAMES:
                traces.append(Trace("registry", name, deploy.LAYERS_KEY))
        return traces

    # ----------------------------------------------------------- daemon ---
    def _scan_daemon(self):
        pid = self.procs.find_pid(DAEMON_EXE)
        if pid is not None:
            return [Trace("daemon", DAEMON_EXE, "pid %d running" % pid)]
        return []

    # ------------------------------------------------------------ temp ----
    def _scan_temp(self):
        traces = []
        seen = set()
        for item in self.temp_paths:
            for p in glob.glob(item):
                key = os.path.normcase(os.path.abspath(p))
                if key in seen or not os.path.isfile(p):
                    continue
                seen.add(key)
                traces.append(Trace("temp_file", p))
        return traces


# ---------------------------------------------------------------- cleaner --
class Cleaner:
    """Removes a Trace list. Re-verifies ownership right before every
    delete: a file that is not ours and not our backup is never deleted
    (a foreign dll parked over our backup gets the backup restored)."""

    def __init__(self, cfg, log, deploy_source_dir=None, registry=None,
                 procs=None):
        self.cfg = cfg
        self.log = log if callable(log) else (lambda _m: None)
        if deploy_source_dir is None:
            dirs = default_source_dirs()
        elif isinstance(deploy_source_dir, str):
            dirs = [deploy_source_dir]
        else:
            dirs = list(deploy_source_dir)
        self.sources = SourceIndex(dirs)
        self.registry = registry or _WinregRegistry()
        self.procs = procs or _procs
        self._failed = 0
        self._config_dst = None
        self._counts = {"removed": 0, "restored": 0, "registry": 0,
                        "temp": 0, "foreign": 0, "daemon": 0, "config": 0}

    def _step(self, step, ok, path, detail=""):
        if not ok:
            self._failed += 1
        self.log(("[clean] %s ok=%d path=%s %s"
                  % (step, 1 if ok else 0, path, detail)).rstrip())

    # -------------------------------------------------------------- api ---
    def remove(self, traces, include_config=False):
        traces = list(traces)
        self._failed = 0
        for t in traces:
            if t.kind == "daemon":
                self._stop_daemon(t)
        self._game_files([t for t in traces
                          if t.kind in ("game_dll", "game_backup")])
        for t in traces:
            if t.kind == "foreign":
                self._counts["foreign"] += 1
                self._step("foreign_keep", True, t.path, t.detail)
        for t in traces:
            if t.kind == "registry":
                self._unregistry(t)
        for t in traces:
            if t.kind == "temp_file":
                self._rm_temp(t)
        if include_config:
            for t in traces:
                if t.kind == "config_dir":
                    self._rename_config(t)
        left = [t.path for t in traces
                if t.kind in ("game_dll", "game_backup", "temp_file")
                and os.path.exists(t.path)]
        ok = self._failed == 0
        c = self._counts
        cfg_txt = ("renamed to " + os.path.basename(self._config_dst)
                   if self._config_dst else "kept")
        summary = ("removed %d file(s), restored %d backup(s), "
                   "%d registry value(s), %d temp file(s); "
                   "daemon stops: %d; foreign left: %d; "
                   "config: %s; failed: %d%s"
                   % (c["removed"], c["restored"], c["registry"], c["temp"],
                      c["daemon"], c["foreign"], cfg_txt, self._failed,
                      ("; still present: %d" % len(left)) if left else ""))
        self.log("[clean] result=%s %s" % ("ok" if ok else "fail", summary))
        return ok, summary

    # ----------------------------------------------------------- daemon ---
    def _stop_daemon(self, t):
        pids = self.procs.find_pids(t.path)
        if not pids:
            self._step("daemon_stop", True, t.path, "already gone")
            return
        ok_all = True
        for pid in pids:
            ok, msg = self.procs.stop_pid(pid)
            self._step("daemon_stop", ok, t.path,
                       "pid %d %s" % (pid, msg))
            ok_all = ok_all and ok
        deadline = time.monotonic() + _DAEMON_STOP_TIMEOUT_S
        while self.procs.find_pid(t.path) is not None \
                and time.monotonic() < deadline:
            time.sleep(0.25)
        alive = self.procs.find_pid(t.path)
        if alive is not None:
            ok, msg = self.procs.stop_pid(alive, force=True)
            self._step("daemon_kill", ok, t.path,
                       "pid %d %s" % (alive, msg))
            ok_all = ok_all and ok
            deadline = time.monotonic() + 2.0
            while self.procs.find_pid(t.path) is not None \
                    and time.monotonic() < deadline:
                time.sleep(0.25)
        gone = self.procs.find_pid(t.path) is None
        if gone:
            self._counts["daemon"] += 1
        self._step("daemon_gone", gone and ok_all, t.path)

    # -------------------------------------------------------- game dirs ---
    def _game_files(self, traces):
        by_dir = {}
        for t in traces:
            by_dir.setdefault(os.path.dirname(t.path), []).append(t)
        for d in sorted(by_dir):
            self._game_dir(d, by_dir[d])

    def _game_dir(self, d, traces):
        dep = Deployer(d)
        # merge target/backup pairs: a game_dll + its game_backup are ONE
        # deploy unit (remove ours, restore the original)
        units = {}
        for t in traces:
            name = os.path.basename(t.path)
            if name.lower().endswith(BACKUP_SUFFIX):
                name = name[:-len(BACKUP_SUFFIX)]
            units.setdefault(name, []).append(t)
        elevated = []
        for name in sorted(units):
            tgt = os.path.join(d, name)
            bak = tgt + BACKUP_SUFFIX
            if name.lower() in KNOWN_DLL_NAMES:
                self._undeploy_known(dep, name, tgt, bak, elevated)
                continue
            # unknown name: only touch what is provably ours
            if os.path.exists(tgt) and self.sources.match(tgt):
                self._remove_file(tgt, elevated)
            if os.path.exists(bak):
                if not os.path.exists(tgt):
                    self._restore_backup(bak, tgt, elevated)
                else:
                    self._step("backup_keep", True, bak,
                               "target present - backup kept")
        for name in elevated:
            try:
                deploy._elevated_remove(d, name)
                self._step("elevated_remove", True, os.path.join(d, name))
            except Exception as e:
                self._step("elevated_remove", False,
                           os.path.join(d, name), str(e))

    def _best_source(self, name, tgt):
        m = self.sources.match(tgt) if os.path.isfile(tgt) else None
        if m:
            return m
        cands = self.sources.sources_for(name)
        return cands[0] if cands else None

    def _undeploy_known(self, dep, name, tgt, bak, elevated):
        had_bak = os.path.exists(bak)
        had_tgt = os.path.exists(tgt)
        if not had_bak and not had_tgt:
            return                              # raced away already
        src = self._best_source(name, tgt)
        try:
            dep.undeploy(name, src or "")       # removes ours, restores bak
        except DeployError as e:
            if isinstance(e.__cause__, PermissionError):
                elevated.append(name)
                self._step("undeploy", False, tgt, "needs elevation")
                return
            if had_bak and os.path.exists(tgt):
                # foreign file parked over our backup -> restore the
                # original over it (nothing foreign is deleted)
                self._restore_backup(bak, tgt, elevated)
                return
            self._step("undeploy", False, tgt, str(e))
            return
        if had_tgt and not os.path.exists(tgt):
            self._counts["removed"] += 1
        if had_bak and not os.path.exists(bak):
            self._counts["restored"] += 1
        self._step("undeploy", True, tgt)

    def _remove_file(self, path, elevated):
        if self.sources.match(path) is None:
            self._step("remove", False, path, "not ours - refused")
            return
        try:
            os.remove(path)
        except PermissionError:
            elevated.append(os.path.basename(path))
            self._step("remove", False, path, "needs elevation")
            return
        except OSError as e:
            self._step("remove", False, path, str(e))
            return
        self._counts["removed"] += 1
        self._step("remove", True, path)

    def _restore_backup(self, bak, tgt, elevated):
        try:
            os.replace(bak, tgt)
        except PermissionError:
            # _elevated_remove(name) deletes tgt then moves bak over it
            elevated.append(os.path.basename(tgt))
            self._step("restore", False, bak, "needs elevation")
            return
        except OSError as e:
            self._step("restore", False, bak, str(e))
            return
        self._counts["restored"] += 1
        self._step("restore", True, bak, "-> " + os.path.basename(tgt))

    # --------------------------------------------------------- registry ---
    def _unregistry(self, t):
        try:
            self.registry.unregister(t.path)
            ok = not self.registry.is_registered(t.path)
        except Exception as e:
            self._step("registry", False, t.path, str(e))
            return
        self._step("registry", ok, t.path)
        if ok:
            self._counts["registry"] += 1

    # ------------------------------------------------------------ temp ----
    def _rm_temp(self, t):
        try:
            os.remove(t.path)
        except FileNotFoundError:
            self._step("temp_remove", True, t.path, "already gone")
            return
        except OSError as e:
            self._step("temp_remove", False, t.path, str(e))
            return
        self._counts["temp"] += 1
        self._step("temp_remove", True, t.path)

    # ----------------------------------------------------------- config ---
    def _rename_config(self, t):
        d = t.path
        stamp = time.strftime("%Y%m%d-%H%M%S")
        dst = d + ".removed-" + stamp
        n = 0
        while os.path.exists(dst):
            n += 1
            dst = "%s.removed-%s-%d" % (d, stamp, n)
        try:
            os.rename(d, dst)
        except OSError as e:
            self._step("config_rename", False, d, str(e))
            return
        self._counts["config"] += 1
        self._config_dst = dst
        self._step("config_rename", True, d, "-> " + dst)
