#!/usr/bin/env python3
"""
DLSS5 Inference Metrics Comparison Tool
Compares model output tensors, rendered BMPs, and audits intermediate activations.
ASCII-only implementation following OOP modular standards.
"""

from dataclasses import dataclass
import argparse
import math
import os
import sys
from typing import Dict, List, Optional, Set, Tuple
import numpy as np


def format_stat_number(val: float, width: int = 12) -> str:
    """Format floating point numbers consistently avoiding table misalignment."""
    if math.isnan(val):
        return "NaN".rjust(width)
    if math.isinf(val):
        s = "+Inf" if val > 0 else "-Inf"
        return s.rjust(width)
    if abs(val) >= 1e5 or (0.0 < abs(val) < 1e-4):
        return f"{val:+{width}.4e}"
    return f"{val:+{width}.4f}"


@dataclass
class HeadMetricsResult:
    """Stores comparative metrics for live_head float32 output tensors."""
    target_path: str
    ref_path: str
    target_elements: int
    ref_elements: int
    target_nans: int
    target_infs: int
    ref_nans: int
    ref_infs: int
    finite_elements: int
    max_diff: float
    mean_diff: float
    mse: float
    rmse: float
    data_range: float
    ref_min: float
    ref_max: float
    psnr_db: float
    correlation: float
    has_nans_or_infs: bool


@dataclass
class BmpMetricsResult:
    """Stores comparative metrics for selftest BMP output images."""
    target_path: str
    ref_path: str
    byte_count: int
    pixel_count: int
    max_pixel_diff: int
    mean_pixel_diff: float
    mse: float
    rmse: float
    psnr_db: float
    exact_matches: int
    exact_pct: float
    within_1: int
    within_1_pct: float


@dataclass
class IntermediateTensorAudit:
    """Stores inspection statistics for intermediate activation tensors."""
    name: str
    path: str
    exists: bool
    dtype_str: str
    element_count: int
    nan_count: int
    inf_count: int
    min_val: float
    max_val: float
    mean_val: float
    status: str


class HeadTensorComparator:
    """Compares live_head float32 output tensors against reference."""

    EXPECTED_SIZE: int = 23396352

    def __init__(self, out_dir: str, target_suffix: str, ref_suffix: str = "fp16") -> None:
        self.out_dir = out_dir
        self.target_suffix = target_suffix
        self.ref_suffix = ref_suffix
        self.target_path = os.path.join(out_dir, f"live_head_{target_suffix}.bin")
        self.ref_path = os.path.join(out_dir, f"live_head_{ref_suffix}.bin")

    def run_comparison(self) -> Tuple[Optional[HeadMetricsResult], Optional[str]]:
        """Loads and computes comparative metrics between target and ref."""
        if not os.path.isfile(self.target_path):
            return None, f"Target file not found: {self.target_path}"
        if not os.path.isfile(self.ref_path):
            return None, f"Reference file not found: {self.ref_path}"

        a = np.fromfile(self.target_path, dtype=np.float32)
        b = np.fromfile(self.ref_path, dtype=np.float32)

        target_nans = int(np.isnan(a).sum())
        target_infs = int(np.isinf(a).sum())
        ref_nans = int(np.isnan(b).sum())
        ref_infs = int(np.isinf(b).sum())
        has_issues = (target_nans > 0 or target_infs > 0 or ref_nans > 0 or ref_infs > 0)

        if a.size != b.size:
            return None, f"Size mismatch: Target has {a.size} elements, Ref has {b.size} elements"

        finite_mask = np.isfinite(a) & np.isfinite(b)
        finite_count = int(np.sum(finite_mask))

        if finite_count == 0:
            return HeadMetricsResult(
                target_path=self.target_path, ref_path=self.ref_path,
                target_elements=a.size, ref_elements=b.size,
                target_nans=target_nans, target_infs=target_infs,
                ref_nans=ref_nans, ref_infs=ref_infs,
                finite_elements=0, max_diff=float("nan"), mean_diff=float("nan"),
                mse=float("nan"), rmse=float("nan"), data_range=float("nan"),
                ref_min=float("nan"), ref_max=float("nan"), psnr_db=0.0,
                correlation=0.0, has_nans_or_infs=True
            ), "All elements contain NaN or Inf"

        a_eval = a[finite_mask] if has_issues else a
        b_eval = b[finite_mask] if has_issues else b

        diff = np.abs(a_eval.astype(np.float64) - b_eval.astype(np.float64))
        max_d = float(np.max(diff))
        mean_d = float(np.mean(diff))

        sq_diff = diff ** 2
        mse = float(np.mean(sq_diff))
        rmse = math.sqrt(mse)

        b_min = float(np.min(b_eval))
        b_max = float(np.max(b_eval))
        data_range = b_max - b_min
        psnr = (20.0 * math.log10(data_range / rmse)) if rmse > 1e-12 else 999.0

        std_a = float(np.std(a_eval))
        std_b = float(np.std(b_eval))
        corr = float(np.corrcoef(a_eval.flatten(), b_eval.flatten())[0, 1]) if (std_a > 1e-12 and std_b > 1e-12) else 0.0

        res = HeadMetricsResult(
            target_path=self.target_path, ref_path=self.ref_path,
            target_elements=a.size, ref_elements=b.size,
            target_nans=target_nans, target_infs=target_infs,
            ref_nans=ref_nans, ref_infs=ref_infs,
            finite_elements=finite_count, max_diff=max_d, mean_diff=mean_d,
            mse=mse, rmse=rmse, data_range=data_range,
            ref_min=b_min, ref_max=b_max, psnr_db=psnr,
            correlation=corr, has_nans_or_infs=has_issues
        )
        return res, None


class BmpComparator:
    """Compares selftest BMP output images skipping the 54-byte header."""

    HEADER_SIZE: int = 54

    def __init__(self, out_dir: str, target_suffix: str, ref_suffix: str = "fp16") -> None:
        self.out_dir = out_dir
        self.target_suffix = target_suffix
        self.ref_suffix = ref_suffix
        self.target_path = os.path.join(out_dir, f"selftest_{target_suffix}.bmp")
        self.ref_path = os.path.join(out_dir, f"selftest_{ref_suffix}.bmp")

    def run_comparison(self) -> Tuple[Optional[BmpMetricsResult], Optional[str]]:
        """Loads BMP pixel buffers and computes pixel difference metrics."""
        if not os.path.isfile(self.target_path):
            return None, f"Target BMP not found: {self.target_path}"
        if not os.path.isfile(self.ref_path):
            return None, f"Reference BMP not found: {self.ref_path}"

        with open(self.target_path, "rb") as f:
            raw_target = f.read()
        with open(self.ref_path, "rb") as f:
            raw_ref = f.read()

        if len(raw_target) <= self.HEADER_SIZE:
            return None, f"Target BMP too small ({len(raw_target)} bytes)"
        if len(raw_ref) <= self.HEADER_SIZE:
            return None, f"Reference BMP too small ({len(raw_ref)} bytes)"

        data_target = raw_target[self.HEADER_SIZE:]
        data_ref = raw_ref[self.HEADER_SIZE:]

        if len(data_target) != len(data_ref):
            return None, (
                f"BMP payload size mismatch: Target has {len(data_target)} bytes, "
                f"Ref has {len(data_ref)} bytes"
            )

        a = np.frombuffer(data_target, dtype=np.uint8)
        b = np.frombuffer(data_ref, dtype=np.uint8)

        diff = np.abs(a.astype(np.int32) - b.astype(np.int32))
        max_d = int(np.max(diff))
        mean_d = float(np.mean(diff.astype(np.float64)))

        sq_diff = (a.astype(np.float64) - b.astype(np.float64)) ** 2
        mse = float(np.mean(sq_diff))
        rmse = math.sqrt(mse)
        psnr = (20.0 * math.log10(255.0 / rmse)) if rmse > 1e-12 else 999.0

        exact_matches = int(np.sum(diff == 0))
        exact_pct = (100.0 * exact_matches / a.size) if a.size > 0 else 0.0

        within_1 = int(np.sum(diff <= 1))
        within_1_pct = (100.0 * within_1 / a.size) if a.size > 0 else 0.0

        res = BmpMetricsResult(
            target_path=self.target_path, ref_path=self.ref_path,
            byte_count=a.size, pixel_count=a.size // 4,
            max_pixel_diff=max_d, mean_pixel_diff=mean_d,
            mse=mse, rmse=rmse, psnr_db=psnr,
            exact_matches=exact_matches, exact_pct=exact_pct,
            within_1=within_1, within_1_pct=within_1_pct
        )
        return res, None


class IntermediateTensorInspector:
    """Inspects intermediate activations in the out directory for NaNs and Infs."""

    DEFAULT_FILES: List[Tuple[str, type]] = [
        ("live_frs.f16", np.float16),
        ("live_b4ds.f16", np.float16),
        ("live_b22ds.f16", np.float16),
        ("live_b38.f16", np.float16),
        ("live_b48.f16", np.float16),
        ("live_b69.f16", np.float16),
        ("live_mrg70.bin", np.float32),
        ("live_head.bin", np.float32),
    ]

    ADDITIONAL_KNOWN_FILES: Dict[str, type] = {
        "live_featV.bin": np.float32,
        "live_x16.bin": np.float16,
        "live_ada.bin": np.float32,
        "live_g2.bin": np.float32,
    }

    def __init__(self, out_dir: str, suffix: Optional[str] = None) -> None:
        self.out_dir = out_dir
        self.suffix = suffix

    def _resolve_candidate_path(self, base_name: str) -> Tuple[str, str]:
        """Find either base filename or suffixed filename if base not present."""
        direct_path = os.path.join(self.out_dir, base_name)
        if os.path.isfile(direct_path):
            return direct_path, base_name

        if self.suffix:
            root, ext = os.path.splitext(base_name)
            suffixed_name = f"{root}_{self.suffix}{ext}"
            suffixed_path = os.path.join(self.out_dir, suffixed_name)
            if os.path.isfile(suffixed_path):
                return suffixed_path, suffixed_name

        return direct_path, base_name

    def inspect_file(self, full_path: str, display_name: str, dtype: type) -> IntermediateTensorAudit:
        """Audits a single tensor file for NaN/Inf values."""
        dtype_str = "float16" if dtype == np.float16 else "float32"

        if not os.path.isfile(full_path):
            return IntermediateTensorAudit(
                name=display_name, path=full_path, exists=False,
                dtype_str=dtype_str, element_count=0, nan_count=0, inf_count=0,
                min_val=0.0, max_val=0.0, mean_val=0.0, status="MISSING"
            )

        data = np.fromfile(full_path, dtype=dtype)
        nan_count = int(np.isnan(data).sum())
        inf_count = int(np.isinf(data).sum())

        if nan_count < data.size:
            min_val = float(np.nanmin(data))
            max_val = float(np.nanmax(data))
            mean_val = float(np.nanmean(data, dtype=np.float64))
        else:
            min_val, max_val, mean_val = float("nan"), float("nan"), float("nan")

        if nan_count == 0 and inf_count == 0:
            status = "OK"
        elif nan_count > 0 and inf_count > 0:
            status = f"FAIL (NaN:{nan_count},Inf:{inf_count})"
        elif nan_count > 0:
            pct = 100.0 * nan_count / data.size if data.size > 0 else 0.0
            status = f"FAIL (NaN:{nan_count} {pct:.1f}%)"
        else:
            status = f"FAIL (Inf:{inf_count})"

        return IntermediateTensorAudit(
            name=display_name, path=full_path, exists=True,
            dtype_str=dtype_str, element_count=data.size,
            nan_count=nan_count, inf_count=inf_count,
            min_val=min_val, max_val=max_val, mean_val=mean_val,
            status=status
        )

    def audit_all(self) -> List[IntermediateTensorAudit]:
        """Audits all specified intermediate files and additional live activations."""
        results: List[IntermediateTensorAudit] = []
        inspected_names: Set[str] = set()

        for base_name, dt in self.DEFAULT_FILES:
            full_path, disp_name = self._resolve_candidate_path(base_name)
            results.append(self.inspect_file(full_path, disp_name, dt))
            inspected_names.add(disp_name)
            inspected_names.add(base_name)

        if os.path.isdir(self.out_dir):
            try:
                for fname in sorted(os.listdir(self.out_dir)):
                    if not fname.startswith("live_") or fname in inspected_names or fname.startswith("live_head_"):
                        continue
                    lower_name = fname.lower()
                    if lower_name.endswith(".f16"):
                        dt = np.float16
                    elif lower_name in self.ADDITIONAL_KNOWN_FILES:
                        dt = self.ADDITIONAL_KNOWN_FILES[lower_name]
                    elif lower_name.endswith(".bin"):
                        dt = np.float32
                    else:
                        continue
                    full_path = os.path.join(self.out_dir, fname)
                    results.append(self.inspect_file(full_path, fname, dt))
                    inspected_names.add(fname)
            except OSError:
                pass

        return results


class MetricsReporter:
    """Formats and prints console reports."""

    LINE_WIDTH: int = 80

    @classmethod
    def print_banner(cls, target_suffix: str, ref_suffix: str, out_dir: str) -> None:
        print("=" * cls.LINE_WIDTH)
        print("                    DLSS5 METRICS COMPARISON REPORT")
        print("=" * cls.LINE_WIDTH)
        print(f"Target Suffix : {target_suffix}")
        print(f"Ref Suffix    : {ref_suffix}")
        print(f"Output Dir    : {out_dir}")
        print("-" * cls.LINE_WIDTH)

    @classmethod
    def print_head_section(cls, res: Optional[HeadMetricsResult], err: Optional[str]) -> None:
        print("\n[1] HEAD TENSOR COMPARISON")
        if err or res is None:
            print(f"  [ERROR] {err}")
            return

        t_base = os.path.basename(res.target_path)
        r_base = os.path.basename(res.ref_path)
        print(f"  Target: {t_base} (float32, {res.target_elements} elements)")
        print(f"  Ref   : {r_base} (float32, {res.ref_elements} elements)")

        if res.target_elements != HeadTensorComparator.EXPECTED_SIZE:
            print(f"  [NOTE] Target count {res.target_elements} != expected {HeadTensorComparator.EXPECTED_SIZE}")

        print("\n  Sanity Checks:")
        t_status = "OK" if (res.target_nans == 0 and res.target_infs == 0) else f"FAIL ({res.target_nans} NaNs, {res.target_infs} Infs)"
        r_status = "OK" if (res.ref_nans == 0 and res.ref_infs == 0) else f"FAIL ({res.ref_nans} NaNs, {res.ref_infs} Infs)"
        print(f"    Target NaNs / Infs : {res.target_nans} / {res.target_infs} ({t_status})")
        print(f"    Ref NaNs / Infs    : {res.ref_nans} / {res.ref_infs} ({r_status})")

        if res.has_nans_or_infs:
            print(f"    [WARNING] Differences computed on {res.finite_elements}/{res.target_elements} finite elements")

        print("\n  Accuracy Metrics:")
        print(f"    Max Difference     : {res.max_diff:.6f}")
        print(f"    Mean Difference    : {res.mean_diff:.6f}")
        print(f"    MSE                : {res.mse:.8e}")
        print(f"    RMSE               : {res.rmse:.6f}")
        print(f"    Data Range (Ref)   : {res.data_range:.6f} [{res.ref_min:+.4f} to {res.ref_max:+.4f}]")
        print(f"    PSNR               : {res.psnr_db:.2f} dB")
        print(f"    Pearson Corr (r)   : {res.correlation:.6f}")

    @classmethod
    def print_bmp_section(cls, res: Optional[BmpMetricsResult], err: Optional[str]) -> None:
        print("\n" + "-" * cls.LINE_WIDTH)
        print("[2] SELFTEST BMP IMAGE COMPARISON")
        if err or res is None:
            print(f"  [ERROR] {err}")
            return

        t_base = os.path.basename(res.target_path)
        r_base = os.path.basename(res.ref_path)
        print(f"  Target: {t_base} ({res.byte_count} bytes, {res.pixel_count} pixels)")
        print(f"  Ref   : {r_base} ({res.byte_count} bytes, {res.pixel_count} pixels)")

        print("\n  Pixel Difference Metrics:")
        print(f"    Max Pixel Diff (0-255)   : {res.max_pixel_diff}")
        print(f"    Mean Pixel Diff          : {res.mean_pixel_diff:.4f}")
        print(f"    Pixel PSNR               : {res.psnr_db:.2f} dB")
        print(f"    Exact Matching Bytes     : {res.exact_matches} / {res.byte_count} ({res.exact_pct:.2f}%)")
        print(f"    Within +/- 1 LSB         : {res.within_1} / {res.byte_count} ({res.within_1_pct:.2f}%)")

    @classmethod
    def print_intermediate_section(cls, audits: List[IntermediateTensorAudit]) -> None:
        print("\n" + "-" * cls.LINE_WIDTH)
        print("[3] INTERMEDIATE TENSOR INTEGRITY CHECK (NaN / Inf Audit)")
        header = f"| {'File':<18} | {'Dtype':<8} | {'Elements':>10} | {'NaNs':>7} | {'Infs':>7} | {'Min':>12} | {'Max':>12} | {'Status':<16} |"
        sep = "+" + "-"*20 + "+" + "-"*10 + "+" + "-"*12 + "+" + "-"*9 + "+" + "-"*9 + "+" + "-"*14 + "+" + "-"*14 + "+" + "-"*18 + "+"

        print(sep)
        print(header)
        print(sep)
        for a in audits:
            if not a.exists:
                min_s, max_s = "--".rjust(12), "--".rjust(12)
                row = f"| {a.name:<18} | {a.dtype_str:<8} | {'--':>10} | {'--':>7} | {'--':>7} | {min_s} | {max_s} | {a.status:<16} |"
            else:
                min_s = format_stat_number(a.min_val, width=12)
                max_s = format_stat_number(a.max_val, width=12)
                row = (
                    f"| {a.name:<18} | {a.dtype_str:<8} | {a.element_count:>10d} | "
                    f"{a.nan_count:>7d} | {a.inf_count:>7d} | {min_s} | {max_s} | {a.status:<16} |"
                )
            print(row)
        print(sep)

    @classmethod
    def print_footer(cls, success: bool) -> None:
        print("\n" + "=" * cls.LINE_WIDTH)
        status_msg = "completed successfully." if success else "completed with warnings or missing artifacts."
        print(f"Status: Comparison {status_msg}")
        print("=" * cls.LINE_WIDTH)


class MetricsComparatorApp:
    """Main application orchestrating the comparison pipeline."""

    def __init__(self, target_suffix: str, ref_suffix: str = "fp16", out_dir: Optional[str] = None) -> None:
        self.target_suffix = self._clean_suffix(target_suffix)
        self.ref_suffix = self._clean_suffix(ref_suffix)
        self.out_dir = self._resolve_out_dir(out_dir)

    @staticmethod
    def _clean_suffix(suffix: str) -> str:
        """Strips prefixes and extensions if passed directly by user."""
        s = suffix.strip()
        for pfx in ("live_head_", "selftest_"):
            if s.startswith(pfx):
                s = s[len(pfx):]
        for ext in (".bin", ".bmp"):
            if s.endswith(ext):
                s = s[:-len(ext)]
        return s

    @staticmethod
    def _resolve_out_dir(custom_dir: Optional[str]) -> str:
        """Finds out directory across common project relative paths."""
        if custom_dir and os.path.isdir(custom_dir):
            return os.path.abspath(custom_dir)

        script_dir = os.path.dirname(os.path.abspath(__file__))
        candidates = [
            custom_dir,
            os.path.join(os.getcwd(), "dlss5", "m11d", "build", "Release", "out"),
            os.path.join(os.getcwd(), "build", "Release", "out"),
            os.path.join(os.getcwd(), "out"),
            os.path.join(script_dir, "..", "dlss5", "m11d", "build", "Release", "out"),
            os.path.join(script_dir, "dlss5", "m11d", "build", "Release", "out"),
        ]
        for c in candidates:
            if c and os.path.isdir(c):
                return os.path.abspath(c)
        return os.path.abspath(os.path.join("dlss5", "m11d", "build", "Release", "out"))

    def run(self) -> int:
        """Runs the comparison workflow."""
        MetricsReporter.print_banner(self.target_suffix, self.ref_suffix, self.out_dir)

        head_comp = HeadTensorComparator(self.out_dir, self.target_suffix, self.ref_suffix)
        head_res, head_err = head_comp.run_comparison()
        MetricsReporter.print_head_section(head_res, head_err)

        bmp_comp = BmpComparator(self.out_dir, self.target_suffix, self.ref_suffix)
        bmp_res, bmp_err = bmp_comp.run_comparison()
        MetricsReporter.print_bmp_section(bmp_res, bmp_err)

        inspector = IntermediateTensorInspector(self.out_dir, self.target_suffix)
        audits = inspector.audit_all()
        MetricsReporter.print_intermediate_section(audits)

        has_failure = (
            (head_res is None or head_res.has_nans_or_infs) or
            (bmp_res is None) or
            any((not a.exists or a.nan_count > 0 or a.inf_count > 0) for a in audits)
        )
        MetricsReporter.print_footer(success=not has_failure)
        return 0 if not has_failure else 1


def main() -> int:
    """CLI entrypoint."""
    parser = argparse.ArgumentParser(
        description="DLSS5 Inference Metrics Comparison Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  python tools/compare_metrics.py pilot0
  python tools/compare_metrics.py pilot4 fp16
  python tools/compare_metrics.py pilot70 --out-dir dlss5/m11d/build/Release/out
"""
    )
    parser.add_argument("suffix", type=str, help="Suffix for target run (e.g. pilot0, pilot4, pilot70)")
    parser.add_argument("ref_suffix", type=str, nargs="?", default="fp16", help="Suffix for reference run (default: fp16)")
    parser.add_argument("--out-dir", "-d", type=str, default=None, help="Custom output directory containing artifacts")

    args = parser.parse_args()
    app = MetricsComparatorApp(target_suffix=args.suffix, ref_suffix=args.ref_suffix, out_dir=args.out_dir)
    return app.run()


if __name__ == "__main__":
    sys.exit(main())
