# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.ui_media - the Media tab: offline DLSS 5 processing of photos and
# videos (no game needed). Layout mirrors ui.py / ui_games.py:
#
#   left    queue listbox ([PHOTO]/[VIDEO] tags) + add/remove/clear +
#           Process all / Stop
#   center  Before / After preview: ZoomView canvases (wheel = zoom
#           anchored at the cursor, drag = pan, double-click = fit) that
#           share one _ViewState so both panes stay region-locked.
#           Underneath: Apply (reprocess now) + Preview frame (video) +
#           Save (write the processed photo via media.SavePolicy).
#   right   the SAME knob set the in-game overlay exposes: blend +
#           sharpness sliders + the Phase30Panel (7 vendor knobs). Turning
#           a knob while a PHOTO is selected debounces DEBOUNCE_MS and
#           reprocesses the loaded original on the worker - the live
#           "overlay for photos" the owner asked for.
#   footer  output dir picker, save-mode combobox (ask/always/subdir),
#           ffmpeg status + Download ffmpeg button.
#
# Preview speed: engine previews run on a DOWNSCALED copy of the source
# (cfg media_preview_max, default 1024 max dim, 0 = full-res) - ~4x fewer
# pixels for 1080p - and the status line says "preview @WxH (approx)".
# Save and Process all always reprocess the stored FULL-res original.
#
# Threading (same rule as everywhere in m13): NO I/O on the Tk thread.
# Engine work, decodes and encodes run inside ctl.submit() jobs; results
# come back through ui.logq ("__call__", callable) marshalling. Every
# preview job carries an increasing serial: a marshalled callback whose
# serial is no longer current is dropped (stale frames never land).
# ============================================================================
import os
import time
import tkinter as tk
from tkinter import filedialog, ttk

from . import media
from .i18n import tr
from .media_img import downscale_nearest, png_b64_rgb, view_sample
from .media_video import VideoJob
from .phase30 import Phase30Panel, PHASE30_VENDOR_KEYS

BG = "#0d1017"
PANEL = "#161a23"
PANEL2 = "#1f2430"
BORDER = "#2a3040"
FG = "#e6e9f0"
MUTED = "#8a91a5"
ACCENT = "#5b8cff"
GREEN = "#3fb950"
RED = "#f85149"
AMBER = "#d29922"

DEBOUNCE_MS = 600
PREV_W, PREV_H = 400, 330
PROGRESS_MIN_S = 0.25


class _ViewState:
    """Shared normalized viewport for the linked Before/After panes.

    `rel` = zoom as a multiplier of each view's own fit-to-box scale
    (REL_MIN..REL_MAX), `cx`/`cy` = the image point under the viewport
    centre, normalized 0..1. Normalized coords keep two DIFFERENT-res
    images (full-res Before, preview-res After) showing the same region,
    so the pair zooms/pans as one."""

    REL_MIN, REL_MAX = 0.25, 8.0

    def __init__(self):
        self.reset()

    def reset(self):
        self.rel = 1.0
        self.cx = 0.5
        self.cy = 0.5

    def clamp_rel(self):
        self.rel = min(max(self.rel, self.REL_MIN), self.REL_MAX)


class ZoomView:
    """One preview pane: caption + a tk.Canvas that renders a crop of the
    stored BGRA image via media_img.view_sample (numpy bilinear).

    Wheel = zoom anchored at the cursor, left-drag = pan, double-click =
    fit-to-box. Views sharing one _ViewState stay region-locked - that is
    the linked Before/After zoom. `on_change` is invoked whenever the
    shared state or the shown image changes (the owner re-renders all
    linked views)."""

    WHEEL_STEP = 1.25

    def __init__(self, parent, title, state, on_change):
        self.state = state
        self._on_change = on_change
        self.img = None                    # (w, h, bgra bytes) or None
        self._tk = None                    # PhotoImage, ref kept alive
        self._drag = None                  # last drag (x, y)
        self._bg = (0x1F, 0x24, 0x30)      # PANEL2 as RGB
        box = tk.Frame(parent, bg=PANEL2, highlightthickness=1,
                       highlightbackground=BORDER,
                       width=PREV_W + 8, height=PREV_H + 26)
        box.pack_propagate(False)
        self.widget = box
        cap = tk.Label(box, text=title, bg=PANEL2, fg=MUTED,
                       font=("Segoe UI", 8, "bold"))
        cap.pack(anchor="w", padx=6, pady=(4, 0))
        self.cv = tk.Canvas(box, bg=PANEL2, highlightthickness=0,
                            cursor="fleur")
        self.cv.pack(fill="both", expand=True, padx=4, pady=4)
        self._item = self.cv.create_image(0, 0, anchor="nw")
        self._txt = self.cv.create_text(
            PREV_W // 2, PREV_H // 2, anchor="center", text="-",
            fill=MUTED, font=("Segoe UI", 9), width=PREV_W - 16,
            justify="center")
        self.cv.bind("<Configure>", lambda _e: self.render())
        self.cv.bind("<MouseWheel>", self._wheel)
        self.cv.bind("<ButtonPress-1>", self._press)
        self.cv.bind("<B1-Motion>", self._drag_to)
        self.cv.bind("<ButtonRelease-1>", self._release)
        self.cv.bind("<Double-Button-1>", self._fit)

    # ---------------------------------------------------------- content --
    def set_image(self, w, h, bgra):
        """Store the raw BGRA frame (bytes ref, zero copy) and render."""
        self.img = (w, h, bgra)
        self.cv.itemconfigure(self._txt, text="")
        self.render()

    def set_text(self, text):
        """Placeholder text instead of an image ('...', an error, '-')."""
        self.img = None
        self._tk = None
        self.cv.itemconfigure(self._item, image="")
        self.cv.itemconfigure(self._txt, text=text)
        self._center_txt()

    def _center_txt(self):
        cw = self.cv.winfo_width()
        ch = self.cv.winfo_height()
        if cw > 4 and ch > 4:
            self.cv.coords(self._txt, cw / 2.0, ch / 2.0)
            self.cv.itemconfigure(self._txt, width=max(cw - 16, 60))

    # ---------------------------------------------------------- render ---
    def _scale(self):
        """screen px per image px = shared rel * this view's fit scale."""
        if self.img is None:
            return 1.0
        w, h, _ = self.img
        cw = max(self.cv.winfo_width(), 1)
        ch = max(self.cv.winfo_height(), 1)
        return self.state.rel * min(cw / w, ch / h)

    def _clamp_state(self):
        """Keep the shared centre over the image (per-view geometry) and
        rel inside REL_MIN..REL_MAX."""
        self.state.clamp_rel()
        if self.img is None:
            return
        w, h, _ = self.img
        z = self._scale()
        cw = max(self.cv.winfo_width(), 1)
        ch = max(self.cv.winfo_height(), 1)
        hx = cw / (2.0 * z * w)
        hy = ch / (2.0 * z * h)
        s = self.state
        s.cx = 0.5 if hx >= 0.5 else min(max(s.cx, hx), 1.0 - hx)
        s.cy = 0.5 if hy >= 0.5 else min(max(s.cy, hy), 1.0 - hy)

    def render(self):
        cw = self.cv.winfo_width()
        ch = self.cv.winfo_height()
        self._center_txt()
        if self.img is None or cw < 8 or ch < 8:
            self.cv.itemconfigure(self._item, image="")
            return
        self._clamp_state()
        w, h, bgra = self.img
        z = self._scale()
        rgb = view_sample(w, h, bgra, z, self.state.cx, self.state.cy,
                          cw, ch, self._bg)
        self._tk = tk.PhotoImage(data=png_b64_rgb(cw, ch, rgb))
        self.cv.itemconfigure(self._item, image=self._tk)

    # ---------------------------------------------------------- events ---
    def _wheel(self, e):
        if self.img is None:
            return
        w, h, _ = self.img
        cw = max(self.cv.winfo_width(), 1)
        ch = max(self.cv.winfo_height(), 1)
        z = self._scale()
        # the normalized image point under the cursor stays put
        ix = self.state.cx + (e.x - cw / 2.0) / (z * w)
        iy = self.state.cy + (e.y - ch / 2.0) / (z * h)
        step = self.WHEEL_STEP if e.delta > 0 else 1.0 / self.WHEEL_STEP
        self.state.rel = min(max(self.state.rel * step,
                                 _ViewState.REL_MIN), _ViewState.REL_MAX)
        z2 = self._scale()
        self.state.cx = ix - (e.x - cw / 2.0) / (z2 * w)
        self.state.cy = iy - (e.y - ch / 2.0) / (z2 * h)
        self._clamp_state()
        self._on_change()

    def _press(self, e):
        self._drag = (e.x, e.y)

    def _drag_to(self, e):
        if self._drag is None or self.img is None:
            return
        dx = e.x - self._drag[0]
        dy = e.y - self._drag[1]
        self._drag = (e.x, e.y)
        w, h, _ = self.img
        z = self._scale()
        self.state.cx -= dx / (z * w)
        self.state.cy -= dy / (z * h)
        self._clamp_state()
        self._on_change()

    def _release(self, _e):
        self._drag = None

    def _fit(self, _e):
        self._drag = None
        self.state.reset()
        self._on_change()

_FILETYPES = [
    ("media", "*.png *.bmp *.jpg *.jpeg *.mp4 *.mkv *.webm *.avi *.mov "
              "*.m4v *.ts *.wmv"),
    ("photos", "*.png *.bmp *.jpg *.jpeg"),
    ("videos", "*.mp4 *.mkv *.webm *.avi *.mov *.m4v *.ts *.wmv"),
    ("all", "*.*"),
]


class MediaTab:
    """The notebook page. `ui` is ManagerUI (ctl, logq, log(), root)."""

    def __init__(self, ui, nb):
        self.ui = ui
        self.ctl = ui.ctl
        self.logq = ui.logq
        self.queue = media.MediaQueue()
        self.ffm = media.Ffmpeg()
        self.engine = None              # media.EngineSession, worker-owned
        self._sel = -1                  # listbox index of the selection
        self._orig = None               # (w, h, bgra) of the loaded photo
        self._orig_path = None
        self._knobs = {}                # last pushed engine knob dict
        self._deb_id = None
        self._vserial = 0               # preview job serial (UI thread only)
        self._load_path = None          # path a load job was queued for
        self._view_path = None          # path the viewers currently show
        self._vstate = _ViewState()     # linked Before/After zoom+pan
        self._render_sched = False      # after_idle render coalescing
        self._prog_ts = 0.0
        self._init_knobs()
        self._build(nb)
        self._ffmpeg_refresh()

    # ------------------------------------------------------------ state ---
    def _init_knobs(self):
        """Seed the knob dict from config: the 7 phase30 keys plus the
        overlay's blend/sharpness. gain mirrors intensity (same convention
        as gamelaunch: DLSS5_GAIN == intensity)."""
        p30 = self.ctl.cfg.get_phase30()
        self._p30_init = p30
        self._knobs.update(p30)
        self._knobs["sharpness"] = float(self.ctl.cfg.get("sharpness")
                                         or 0.8)
        self._knobs["blend"] = float(self.ctl.cfg.get("blend") or 1.0)
        self._knobs["gain"] = float(p30.get("intensity", 1.0))

    def _call_ui(self, fn):
        """Marshal a callable back to the Tk thread (ui.py drains the
        ("__call__", fn) sentinel from logq)."""
        self.logq.put(("__call__", fn))

    # ------------------------------------------------------------ build ---
    def _build(self, nb):
        t = ttk.Frame(nb, padding=8)
        nb.add(t, text=tr("tab_media"))
        self.frame = t

        main = ttk.Frame(t)
        main.pack(fill="both", expand=True)

        self._build_queue_col(main)
        self._build_preview_col(main)
        self._build_knob_col(main)
        self._build_footer(t)

    def _build_queue_col(self, main):
        left = ttk.Frame(main)
        left.pack(side="left", fill="y", padx=(0, 8))

        listbox_wrap = ttk.Frame(left)
        listbox_wrap.pack(fill="both", expand=True)
        self.lst = tk.Listbox(listbox_wrap, width=34, bg=PANEL, fg=FG,
                              selectbackground=ACCENT,
                              selectforeground="#ffffff", relief="flat",
                              bd=4, highlightthickness=1,
                              highlightcolor=BORDER,
                              highlightbackground=BORDER,
                              activestyle="none", exportselection=False,
                              font=("Consolas", 9))
        sb = ttk.Scrollbar(listbox_wrap, orient="vertical",
                           command=self.lst.yview)
        self.lst.configure(yscrollcommand=sb.set)
        self.lst.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.lst.bind("<<ListboxSelect>>", self._on_select)

        btns = ttk.Frame(left)
        btns.pack(fill="x", pady=(4, 0))
        ttk.Button(btns, text=tr("media_add_files"),
                   command=self._add_files).pack(side="left")
        ttk.Button(btns, text=tr("media_add_folder"),
                   command=self._add_folder).pack(side="left", padx=4)
        ttk.Button(btns, text=tr("media_remove"),
                   command=self._remove_sel).pack(side="left", padx=4)
        ttk.Button(btns, text=tr("media_clear"),
                   command=self._clear).pack(side="left")

        proc = ttk.Frame(left)
        proc.pack(fill="x", pady=(6, 0))
        ttk.Button(proc, text=tr("media_process"), style="Accent.TButton",
                   command=self._process_all).pack(side="left")
        ttk.Button(proc, text=tr("media_stop"), style="Danger.TButton",
                   command=self._stop).pack(side="left", padx=6)
        self.status_lbl = ttk.Label(left, text="", style="Muted.TLabel",
                                    wraplength=270, justify="left")
        self.status_lbl.pack(anchor="w", pady=(6, 0))
        self.prog_lbl = ttk.Label(left, text="", style="Muted.TLabel")
        self.prog_lbl.pack(anchor="w")

    def _build_preview_col(self, main):
        center = ttk.Frame(main)
        center.pack(side="left", fill="both", expand=True, padx=(0, 8))
        boxes = ttk.Frame(center)
        boxes.pack(fill="both", expand=True)
        self.before_view = self._preview_box(
            boxes, tr("media_before"), side="left")
        self.after_view = self._preview_box(
            boxes, tr("media_after"), side="right")
        self._views = (self.before_view, self.after_view)
        row = ttk.Frame(center)
        row.pack(fill="x", pady=(6, 0))
        self.apply_btn = ttk.Button(row, text=tr("media_apply"),
                                    command=self._apply_now)
        self.apply_btn.pack(side="left")
        self.prev_btn = ttk.Button(row, text=tr("media_preview_btn"),
                                   command=self._preview_frame)
        self.prev_btn.state(["disabled"])
        self.prev_btn.pack(side="left", padx=(6, 0))
        ttk.Label(row, text=tr("media_zoom_fit"),
                  style="Muted.TLabel").pack(side="left", padx=10)
        self.save_btn = ttk.Button(row, text=tr("media_save"),
                                   command=self._save_current)
        self.save_btn.pack(side="right")

    def _preview_box(self, parent, title, side):
        view = ZoomView(parent, title, self._vstate, self._views_changed)
        view.widget.pack(side=side, fill="both", expand=True,
                         padx=(0, 6) if side == "left" else (6, 0))
        return view

    def _views_changed(self):
        """Zoom/pan state moved: re-render BOTH panes (region-locked),
        coalesced through after_idle so a drag costs one render pair."""
        if self._render_sched:
            return
        self._render_sched = True
        self.ui.root.after_idle(self._render_views)

    def _render_views(self):
        self._render_sched = False
        for v in self._views:
            v.render()

    def _build_knob_col(self, main):
        right = ttk.Frame(main)
        right.pack(side="right", fill="y")

        bs = tk.Frame(right, bg=BG)
        bs.pack(fill="x", pady=(0, 4))
        self.blend_var = tk.DoubleVar(value=self._knobs["blend"])
        self._add_slider(bs, tr("ov_blend"), self.blend_var, 0.0, 1.0,
                         "blend").pack(side="left", fill="x", expand=True,
                                       padx=(0, 6))
        self.sharp_var = tk.DoubleVar(value=self._knobs["sharpness"])
        self._add_slider(bs, tr("sharpness"), self.sharp_var, 0.0, 1.0,
                         "sharpness").pack(side="right", fill="x",
                                          expand=True, padx=(6, 0))

        wrap = tk.Frame(right, bg=PANEL, highlightthickness=1,
                        highlightbackground=BORDER)
        wrap.pack(fill="x", pady=(2, 4))
        self.p30 = Phase30Panel(
            wrap, initial_values=self._p30_init,
            on_change=self._knob_changed, on_reset=self._knobs_reset,
            bg=PANEL, fg=FG, accent=ACCENT, panel2=PANEL2, muted=MUTED,
            compact=True)
        self.p30.pack(fill="x", padx=8, pady=6)

    def _add_slider(self, parent, label, var, lo, hi, key):
        box = tk.Frame(parent, bg=BG)
        hdr = tk.Frame(box, bg=BG)
        hdr.pack(fill="x")
        tk.Label(hdr, text=label, bg=BG, fg=FG,
                 font=("Segoe UI", 8, "bold")).pack(side="left")
        val_lbl = tk.Label(hdr, text="%.2f" % var.get(), bg=BG, fg=ACCENT,
                           font=("Consolas", 8, "bold"))
        val_lbl.pack(side="right")

        def _upd(_v):
            v = round(float(var.get()), 3)
            val_lbl.config(text="%.2f" % v)
            self._knob_changed(key, v)

        tk.Scale(box, from_=lo, to=hi, resolution=0.05,
                 orient="horizontal", variable=var, command=_upd,
                 bg=BG, fg=FG, troughcolor=PANEL2, highlightthickness=0,
                 bd=0, activebackground=ACCENT, showvalue=False,
                 sliderrelief="flat").pack(fill="x")
        return box

    def _build_footer(self, t):
        foot = ttk.Frame(t)
        foot.pack(fill="x", pady=(6, 0))
        ttk.Label(foot, text=tr("media_outdir")).pack(side="left")
        self.outdir_var = tk.StringVar(
            value=self.ctl.cfg.get("media_out_dir") or "")
        outdir_entry = ttk.Entry(foot, textvariable=self.outdir_var,
                               width=34)
        outdir_entry.pack(side="left", padx=4)
        outdir_entry.bind("<FocusOut>", lambda _e: self.ctl.cfg.set(
            "media_out_dir", self.outdir_var.get() or None))
        ttk.Button(foot, text=tr("browse"),
                   command=self._pick_outdir).pack(side="left")
        ttk.Label(foot, text=tr("media_save_mode")).pack(side="left",
                                                       padx=(10, 0))
        self._mode_labels = [tr("media_mode_ask"), tr("media_mode_always"),
                             tr("media_mode_subdir")]
        self._mode_keys = ["ask", "always", "subdir"]
        cur = self.ctl.cfg.get("media_save_mode") or "subdir"
        self.mode_var = tk.StringVar(
            value=self._mode_labels[self._mode_keys.index(cur)]
            if cur in self._mode_keys else self._mode_labels[2])
        self.mode_cb = ttk.Combobox(foot, textvariable=self.mode_var,
                                    values=self._mode_labels, width=26,
                                    state="readonly")
        self.mode_cb.pack(side="left", padx=4)
        self.mode_cb.bind("<<ComboboxSelected>>", self._mode_changed)

        self.ff_lbl = ttk.Label(foot, text="", style="Muted.TLabel")
        self.ff_lbl.pack(side="right")
        self.ff_btn = ttk.Button(foot, text=tr("media_dl_ffmpeg"),
                                 command=self._download_ffmpeg)
        # packed on demand by _ffmpeg_refresh()

    # ------------------------------------------------------- queue edits --
    def _add_files(self):
        paths_ = filedialog.askopenfilenames(
            title=tr("media_add_files"), filetypes=_FILETYPES)
        if not paths_:
            return
        n = self.queue.add_many(paths_)
        self._refresh_list()
        self._status("+%d" % n)
        self._select_first_new_photo(paths_)

    def _add_folder(self):
        d = filedialog.askdirectory(title=tr("media_add_folder"))
        if not d:
            return
        n = self.queue.add_folder(d)
        self._refresh_list()
        self._status("+%d" % n)
        self._select_first_new_photo(None)

    def _select_first_new_photo(self, added_paths):
        """Auto-preview: select the first newly queued PHOTO so its
        before/after preview runs without a manual click. `added_paths`
        narrows the search to the files just added (None = rescan all)."""
        wanted = None
        if added_paths:
            wanted = {os.path.normcase(os.path.abspath(p))
                      for p in added_paths}
        for idx, it in enumerate(self.queue.items):
            if it.kind != "photo":
                continue
            if wanted is not None and \
                    os.path.normcase(it.path) not in wanted:
                continue
            if self._sel == idx and self._load_path == it.path:
                return                      # already previewing it
            self.lst.selection_clear(0, "end")
            self.lst.selection_set(idx)
            self.lst.see(idx)
            # <<ListboxSelect>> fires on selection_set; call the handler
            # too - a no-op if the event already ran (same-path dedupe).
            self._on_select()
            return

    def _remove_sel(self):
        sel = self.lst.curselection()
        if not sel:
            return
        item = self.queue.items[sel[0]]
        self.queue.remove(item.path)
        self._sel = -1
        if self._orig_path == item.path or self._load_path == item.path:
            # the previewed photo is gone: drop in-flight results + panes
            self._invalidate_preview()
            self._set_text(self.before_view, "-")
            self._set_text(self.after_view, "-")
        else:
            self._vserial += 1              # indices shifted; stale anyway
        self._refresh_list()

    def _clear(self):
        self.queue.clear()
        self._sel = -1
        self._invalidate_preview()
        self._set_text(self.before_view, "-")
        self._set_text(self.after_view, "-")
        self._refresh_list()

    _STATUS_KEY = {"working": "media_status_work",
                   "done": "media_status_done",
                   "error": "media_status_error",
                   "skipped": "media_status_skip",
                   "cancelled": "media_status_cancel"}

    def _row_text(self, item):
        tag = tr("media_photo_tag") if item.kind == "photo" \
            else tr("media_video_tag")
        tail = ""
        if item.status != "queued":
            tail = "  [%s]" % tr(self._STATUS_KEY.get(item.status,
                                                    "media_status_error"))
        return "[%s] %s%s" % (tag, os.path.basename(item.path), tail)

    def _refresh_list(self):
        self.lst.delete(0, "end")
        for it in self.queue.items:
            self.lst.insert("end", self._row_text(it))

    def _refresh_row(self, item):
        try:
            idx = self.queue.items.index(item)
        except ValueError:
            return
        self.lst.delete(idx)
        self.lst.insert(idx, self._row_text(item))
        if idx == self._sel:
            self.lst.selection_set(idx)

    # --------------------------------------------------------- selection --
    def _selected_item(self):
        sel = self.lst.curselection()
        if not sel or sel[0] >= len(self.queue.items):
            return None
        return self.queue.items[sel[0]]

    def _new_serial(self):
        """UI thread: bump the preview serial. Any marshalled callback or
        queued job carrying an older serial is dropped/skipped - stale
        previews never overwrite a newer selection."""
        self._vserial += 1
        return self._vserial

    def _invalidate_preview(self):
        """Selection gone or changed: pending results belong to the old
        context - make every in-flight job stale."""
        self._new_serial()
        self._load_path = None
        self._orig = None
        self._orig_path = None
        self._view_path = None

    def _on_select(self, _e=None):
        sel = self.lst.curselection()
        item = self._selected_item()
        if item is None:
            self._sel = -1
            return
        self.prev_btn.state(
            ["!disabled"] if item.kind == "video" else ["disabled"])
        if item.kind == "photo":
            idx = sel[0] if sel else -1
            if self._sel == idx and self._load_path == item.path:
                return                      # same photo already queued
            self._sel = idx
            self._queue_load(item)
        else:
            self._sel = sel[0] if sel else -1
            self._invalidate_preview()
            self._set_text(self.before_view, tr("media_video_tag"))
            self._set_text(self.after_view, "-")

    def _queue_load(self, item):
        """Queue a photo decode+preview job on the worker."""
        self._load_path = item.path
        self._orig = None
        self._orig_path = None
        self._set_text(self.before_view, "...")
        self._set_text(self.after_view, "...")
        serial = self._new_serial()
        self._status(tr("media_processing"))
        self.ctl.submit(self._job_load_photo, item.path, serial,
                        dict(self._knobs))   # UI-side knobs snapshot

    # --------------------------------------------------------- preview ----
    def _engine(self):
        """WORKER THREAD ONLY: lazily create the one EngineSession."""
        if self.engine is None:
            self.engine = media.EngineSession(cfg=self.ctl.cfg)
        return self.engine

    def engine_close(self):
        """Submitted as a worker job on app shutdown (ui.py _on_close);
        queued FIFO so in-flight media work finishes first."""
        if self.engine is not None:
            self.engine.close()
        return True, "media engine closed"

    def _preview_max(self):
        """cfg media_preview_max: biggest source dim fed to the engine for
        previews (0 = full-res). Bad/missing values fall back to 1024."""
        try:
            v = self.ctl.cfg.get("media_preview_max", 1024)
            if v is None:
                return 1024
            return max(0, int(v))
        except (TypeError, ValueError):
            return 1024

    def _job_load_photo(self, path, serial, knobs):
        """Worker job: decode -> Before preview, then engine -> After.
        The serial is re-checked between stages: a superseded job bails
        before burning GPU time."""
        if serial != self._vserial:
            return False, "preview: stale selection"
        try:
            w, h, bgra = media.PhotoIO.load(path, self.ffm)
        except Exception as e:
            self._call_ui(lambda: self._ui_err_before(
                serial, "load: %s" % e))
            return False, "load failed: %s" % e
        if serial != self._vserial:
            return False, "preview: stale selection"
        self._orig = (w, h, bgra)
        self._orig_path = path
        self._call_ui(lambda: self._ui_show_before(serial, path, w, h, bgra))
        return self._job_reprocess(path, serial, knobs)

    def _job_reprocess(self, path, serial, knobs):
        """Worker job: run the loaded original through the engine at
        PREVIEW resolution (media_preview_max) and show it as After.
        Save/Process all use the full-res source - this is the fast,
        explicitly approximate preview path."""
        if serial != self._vserial or \
                self._orig is None or self._orig_path != path:
            return False, "preview: stale selection"
        w, h, bgra = self._orig
        pmax = self._preview_max()
        if pmax > 0:
            pw, ph, pbgra = downscale_nearest(w, h, bgra, pmax, pmax)
        else:
            pw, ph, pbgra = w, h, bgra
        try:
            eng = self._engine()
            eng.apply_knobs(knobs)
            out = eng.process(pbgra, pw, ph)
        except Exception as e:
            self._call_ui(lambda: self._ui_err_after(
                serial, "engine: %s" % e))
            return False, "engine: %s" % e
        approx = (pw, ph) != (w, h)
        self._call_ui(lambda: self._ui_show_after(
            serial, pw, ph, out, approx))
        return True, "preview: %dx%d -> %dx%d" % (w, h, pw, ph)

    def _preview_frame(self):
        item = self._selected_item()
        if item is None or item.kind != "video":
            return
        self._set_text(self.before_view, "...")
        self._set_text(self.after_view, "...")
        serial = self._new_serial()
        self._status(tr("media_processing"))
        self.ctl.submit(self._job_video_frame, item.path, serial,
                        dict(self._knobs))

    def _job_video_frame(self, path, serial, knobs):
        """Worker job: decode frame 0 of a video, process it at preview
        resolution, show both panes."""
        if serial != self._vserial:
            return False, "preview: stale selection"
        ffmpeg, ffprobe = self.ffm.resolve(), self.ffm.ffprobe()
        if not ffmpeg or not ffprobe:
            return False, tr("media_ffmpeg_missing")
        try:
            job = VideoJob(ffmpeg, ffprobe, path, "-", self._engine())
            w, h, frame = job.grab_frame(0)
        except Exception as e:
            return False, "preview: %s" % e
        if serial != self._vserial:
            return False, "preview: stale selection"
        self._call_ui(lambda: self._ui_show_before(serial, path, w, h, frame))
        pmax = self._preview_max()
        if pmax > 0:
            pw, ph, pbgra = downscale_nearest(w, h, frame, pmax, pmax)
        else:
            pw, ph, pbgra = w, h, frame
        try:
            eng = self._engine()
            eng.apply_knobs(knobs)
            out = eng.process(pbgra, pw, ph)
        except Exception as e:
            self._call_ui(lambda: self._ui_err_after(
                serial, "engine: %s" % e))
            return False, "engine: %s" % e
        approx = (pw, ph) != (w, h)
        self._call_ui(lambda: self._ui_show_after(
            serial, pw, ph, out, approx))
        return True, "frame0 %dx%d -> %dx%d" % (w, h, pw, ph)

    # ---------------------------------------------- marshalled UI landing --
    def _ui_show_before(self, serial, path, w, h, bgra):
        if serial != self._vserial:
            return
        if path != self._view_path:
            self._vstate.reset()            # new image -> fit-to-box
            self._view_path = path
        self._set_image(self.before_view, w, h, bgra)

    def _ui_show_after(self, serial, w, h, bgra, approx):
        if serial != self._vserial:
            return
        self._set_image(self.after_view, w, h, bgra)
        if approx:
            self._status(tr("media_preview_approx", w, h))
        else:
            self._status(tr("media_preview_size", w, h))

    def _ui_err_before(self, serial, msg):
        if serial != self._vserial:
            return
        self._load_path = None          # failed: Apply may retry the load
        self._set_text(self.before_view, msg)
        self._set_text(self.after_view, "-")
        self._status(msg)

    def _ui_err_after(self, serial, msg):
        if serial != self._vserial:
            return
        self._set_text(self.after_view, msg)
        self._status(msg)

    def _set_image(self, view, w, h, bgra):
        """Store + show a BGRA frame in a ZoomView. (Contract changed from
        the old (label, png_b64) thumbnail version; tests exercise the
        worker/render helpers in media_img, not Tk widgets.)"""
        view.set_image(w, h, bgra)

    def _set_text(self, view, text):
        view.set_text(text)

    # ------------------------------------------------------------- knobs --
    def _knob_changed(self, key, val):
        self._knobs[key] = val
        if key == "intensity":
            self._knobs["gain"] = float(val)   # gain mirrors intensity
        if self._deb_id is not None:
            self.ui.root.after_cancel(self._deb_id)
        self._deb_id = self.ui.root.after(DEBOUNCE_MS,
                                          self._debounced_preview)

    def _knobs_reset(self):
        """Phase30Panel already reset its own vars; mirror the 7 vendor
        knobs into _knobs and reprocess. blend/sharpness stay untouched
        (get_values() also carries the live-file engine knobs - those are
        the panel's pass-through defaults, not a user intent to reset)."""
        p30 = {k: v for k, v in self.p30.get_values().items()
               if k in PHASE30_VENDOR_KEYS}
        self._knobs.update(p30)
        self._knobs["gain"] = float(p30.get("intensity", 1.0))
        self._debounced_preview()

    def _debounced_preview(self):
        """DEBOUNCE_MS after the last knob move: reprocess the selected
        photo on the worker (the live 'overlay for photos')."""
        self._deb_id = None
        self._request_reprocess()

    def _request_reprocess(self):
        """Queue a reprocess job for the selected photo (if one is loaded
        and still selected). The serial stamps it; stale jobs self-drop
        before touching the engine."""
        item = self._selected_item()
        if item is None or item.kind != "photo":
            return
        if self._orig is None or self._orig_path != item.path:
            return
        serial = self._new_serial()
        self._status(tr("media_processing"))
        self.ctl.submit(self._job_reprocess, item.path, serial,
                        dict(self._knobs))

    def _apply_now(self):
        """Apply button: explicitly re-run the selected item through the
        engine with the current knobs (photos: reprocess the loaded
        original; videos: re-grab frame 0). Cancels a pending debounce."""
        if self._deb_id is not None:
            self.ui.root.after_cancel(self._deb_id)
            self._deb_id = None
        item = self._selected_item()
        if item is None:
            return
        if item.kind == "video":
            self._preview_frame()
            return
        if self._orig is None or self._orig_path != item.path:
            if self._load_path != item.path:
                self._queue_load(item)      # never loaded / failed: retry
            return                          # else a load is already queued
        self._request_reprocess()

    # -------------------------------------------------------------- save --
    def _save_current(self):
        """Save button under the After pane: write the SELECTED photo
        processed at FULL resolution through media.SavePolicy rules:
        'ask' -> save-as dialog, 'always' -> media_out_dir, 'subdir' ->
        <source dir>/dlss5. Never overwrites the source file."""
        item = self._selected_item()
        if item is None or item.kind != "photo":
            self._status(tr("media_save_photo_only"))
            return
        if self._orig is None or self._orig_path != item.path:
            self._status(tr("media_save_wait"))
            return
        w, h, bgra = self._orig             # snapshot the full-res source
        policy = media.SavePolicy(self.ctl.cfg)
        if policy.mode == "ask":
            dst = filedialog.asksaveasfilename(
                title=tr("media_save"),
                initialdir=os.path.dirname(item.path),
                initialfile=media.SavePolicy.output_name(item.path,
                                                         "photo"),
                defaultextension=".png",
                filetypes=[("PNG", "*.png"), ("BMP", "*.bmp")])
            if not dst:
                return
            if os.path.normcase(os.path.abspath(dst)) == \
                    os.path.normcase(item.path):
                self._status("refusing to overwrite source")
                return
        else:
            try:
                dst = policy.resolve(item.path, "photo")
            except media.MediaError as e:
                self._status(str(e))
                return
        knobs = dict(self._knobs)
        self._status(tr("media_processing"))
        self.ctl.submit(self._job_save_photo, w, h, bgra, dst, knobs)

    def _job_save_photo(self, w, h, bgra, dst, knobs):
        """Worker job: FULL-res engine pass + PNG/BMP encode + write."""
        try:
            eng = self._engine()
            eng.apply_knobs(knobs)
            out = eng.process(bgra, w, h)
            media.PhotoIO.save(dst, w, h, out)
        except Exception as e:
            self._call_ui(lambda: self._status("save: %s" % e))
            return False, "save: %s" % e
        self._call_ui(lambda: self._status(tr("media_saved", dst)))
        return True, "saved: %s" % dst

    # ------------------------------------------------------------- batch --
    def _save_mode_key(self):
        try:
            return self._mode_keys[self._mode_labels.index(
                self.mode_var.get())]
        except ValueError:
            return "subdir"

    def _process_all(self):
        if not self.queue.items:
            self._status(tr("media_queue_empty"))
            return
        batch_dir = None
        if self._save_mode_key() == "ask":
            batch_dir = filedialog.askdirectory(
                title=tr("media_pick_out"))
            if not batch_dir:
                return
        self.queue.reset()
        for it in self.queue.items:
            if it.status != "done":
                it.status = "queued"
        self._refresh_list()
        self.ctl.submit(self._job_run_all, batch_dir)

    def _job_run_all(self, batch_dir):
        """Worker job: the whole queue, sequentially."""
        try:
            eng = self._engine()
        except Exception as e:
            return False, "engine: %s" % e
        policy = media.SavePolicy(self.ctl.cfg)
        knobs = dict(self._knobs)

        def prog(item, i, total):
            now = time.monotonic()
            if now - self._prog_ts < PROGRESS_MIN_S:
                return
            self._prog_ts = now
            name = os.path.basename(item.path)
            self._call_ui(lambda: self.prog_lbl.config(
                text=tr("media_progress", name, i, total)))

        def done(item):
            self._call_ui(lambda: self._refresh_row(item))

        ok, msg = self.queue.run_all(
            eng, policy, self.ffm, knobs=knobs, batch_dir=batch_dir,
            progress_cb=prog, item_cb=done,
            log=lambda m: self.ui.log("mgr", m))
        self._call_ui(lambda: self._batch_done(ok, msg))
        return ok, tr("media_done", msg)

    def _batch_done(self, ok, msg):
        self._refresh_list()
        self.prog_lbl.config(text="")
        self._status(tr("media_done", msg))

    def _stop(self):
        self.queue.cancel()
        self._status(tr("media_status_cancel"))

    # ------------------------------------------------------- settings row --
    def _pick_outdir(self):
        d = filedialog.askdirectory(title=tr("media_outdir"))
        if d:
            self.outdir_var.set(d)
            self.ctl.cfg.set("media_out_dir", d)

    def _mode_changed(self, _e=None):
        self.ctl.cfg.set("media_save_mode", self._save_mode_key())

    def _ffmpeg_refresh(self):
        exe = self.ffm.resolve()
        if exe:
            self.ff_lbl.config(text=tr("media_ffmpeg_ok", exe))
            try:
                self.ff_btn.pack_forget()
            except tk.TclError:
                pass
        else:
            self.ff_lbl.config(text=tr("media_ffmpeg_missing"))
            if not self.ff_btn.winfo_ismapped():
                self.ff_btn.pack(side="right", padx=6)

    def _download_ffmpeg(self):
        self.ff_btn.state(["disabled"])
        self.ff_lbl.config(text=tr("media_dl_busy"))
        self.ctl.submit(self._job_download_ffmpeg)

    def _job_download_ffmpeg(self):
        try:
            exe = self.ffm.download()
        except Exception as e:
            self._call_ui(lambda: self._ffmpeg_failed(str(e)))
            return False, tr("media_dl_fail", e)
        self._call_ui(self._ffmpeg_refresh)
        return True, tr("media_dl_done", exe)

    def _ffmpeg_failed(self, err):
        self.ff_lbl.config(text=tr("media_dl_fail", err))
        self.ff_btn.state(["!disabled"])

    # ------------------------------------------------------------- misc ---
    def _status(self, text):
        self.status_lbl.config(text=text)
