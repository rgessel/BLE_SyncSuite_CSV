#!/usr/bin/env python3
"""
Plot CheepSync linear fit from an exported BLE Sync Suite CSV.

The app exports columns: device_address, device_name, seq, t_us, received_at_ns
(CsvExport.kt). CheepSync uses least-squares regression on a sliding window over
(beacon time in ns, receiver time in ns):  Tr ≈ α + β * tb  (see sync/CheepSync.kt).

β is dimensionless (receiver ns per beacon ns). β≈1 means matched clock rates; skew
from unity is often reported as (β−1)×1e6 ppm.

The main comparison uses deviation from a β=1 clock (anchored at the first sample): the
unity reference is y=0; CheepSync is the red curve (ms). αᵢ and βᵢ are recomputed after
each sample (sliding window), matching the app—not a single final α,β for the whole file.
That avoids two nearly parallel diagonals on a 10^11 ns scale.

App logging (BleManager.kt): t_us is uint64 LE from the notification payload; received_at_ns
is SystemClock.elapsedRealtimeNanos() when the packet is handled—same pair CheepSync uses.

If the CSV includes cheep_sync_alpha_ns and cheep_sync_beta (per-row snapshot from the app),
the script overlays predictions from those columns and adds a panel: replay vs CSV prediction
difference (should be ~0 if the export matches CheepSync math and ordering).

Use --logging-checks to write a second figure: Δt_us vs nominal beacon step, and (when
synced_at_ns is present) synced−receive and synced−CheepSync prediction for export QA.

Dependencies: pip install matplotlib numpy
"""

from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


DEFAULT_WINDOW = 50


def cheep_sync_replay(
    t_us: np.ndarray,
    received_at_ns: np.ndarray,
    window_size: int = DEFAULT_WINDOW,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """
    Replay CheepSync.addSample for every row (same order as the app).

    After each sample is added to the sliding window, recomputes α, β and stores
    prediction at *this* beacon time: pred[k] = α[k] + β[k]·t_beacon_ns[k].

    Returns (alpha, beta, pred, rms_window_ms), each length n, nan until the first
    valid regression (≥2 samples, nonzero var(tb)).
    """
    n = int(t_us.shape[0])
    tb_all = t_us.astype(np.float64) * 1000.0
    tr_all = received_at_ns.astype(np.float64)

    alpha_arr = np.full(n, np.nan, dtype=np.float64)
    beta_arr = np.full(n, np.nan, dtype=np.float64)
    pred_arr = np.full(n, np.nan, dtype=np.float64)
    rms_win_arr = np.full(n, np.nan, dtype=np.float64)

    window_tb: list[float] = []
    window_tr: list[float] = []

    for k in range(n):
        tb_ns = float(tb_all[k])
        tr_ns = float(tr_all[k])
        window_tb.append(tb_ns)
        window_tr.append(tr_ns)
        while len(window_tb) > window_size:
            window_tb.pop(0)
            window_tr.pop(0)

        if len(window_tb) < 2:
            continue

        tb = np.array(window_tb, dtype=np.float64)
        tr = np.array(window_tr, dtype=np.float64)
        nw = float(tb.size)
        tb_mean = tb.mean()
        tr_mean = tr.mean()
        x = tb - tb_mean
        y = tr - tr_mean
        var_tb = float(np.dot(x, x))
        if var_tb == 0.0:
            continue
        cov = float(np.dot(x, y))
        beta = cov / var_tb
        alpha = tr_mean - beta * tb_mean
        pred = alpha + beta * tb
        rss = float(np.sum((tr - pred) ** 2))
        rms_ms = math.sqrt(rss / nw) / 1_000_000.0

        alpha_arr[k] = alpha
        beta_arr[k] = beta
        pred_arr[k] = alpha + beta * tb_ns
        rms_win_arr[k] = rms_ms

    return alpha_arr, beta_arr, pred_arr, rms_win_arr


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


COL_ALPHA = "cheep_sync_alpha_ns"
COL_BETA = "cheep_sync_beta"
COL_SYNCED = "synced_at_ns"


def csv_has_logged_fit(rows: list[dict[str, str]]) -> bool:
    if not rows:
        return False
    k = rows[0].keys()
    return COL_ALPHA in k and COL_BETA in k


def parse_logged_fit(
    block: list[dict[str, str]],
) -> tuple[np.ndarray | None, np.ndarray | None]:
    """Per-row α, β from CSV, or (None, None) if columns absent."""
    if not csv_has_logged_fit(block):
        return None, None
    def f64(r: dict[str, str], key: str) -> float:
        s = r.get(key)
        if s is None or (isinstance(s, str) and not s.strip()):
            return float("nan")
        return float(s)

    a = np.array([f64(r, COL_ALPHA) for r in block], dtype=np.float64)
    b = np.array([f64(r, COL_BETA) for r in block], dtype=np.float64)
    return a, b


def csv_has_synced(rows: list[dict[str, str]]) -> bool:
    if not rows:
        return False
    return COL_SYNCED in rows[0].keys()


def _ylim_percentile(
    y: np.ndarray, lo_q: float = 2.0, hi_q: float = 98.0
) -> tuple[float, float]:
    m = np.isfinite(y)
    if not np.any(m):
        return -1.0, 1.0
    lo, hi = np.percentile(y[m], [lo_q, hi_q])
    pad = max(0.08 * (hi - lo), 1e-9)
    return float(lo - pad), float(hi + pad)


def parse_synced_ns(block: list[dict[str, str]]) -> np.ndarray | None:
    if not csv_has_synced(block):
        return None
    out: list[float] = []
    for r in block:
        s = r.get(COL_SYNCED)
        if s is None or (isinstance(s, str) and not str(s).strip()):
            out.append(float("nan"))
        else:
            out.append(float(s))
    return np.array(out, dtype=np.float64)


def plot_logging_checks_figure(
    by_addr: dict[str, list[dict[str, str]]],
    devices: list[str],
    rows: list[dict[str, str]],
    *,
    output: Path | None,
    show: bool,
) -> None:
    """
    Second figure: QA plots for t_us stepping and synced_at_ns (if exported).
    Uses same receive-time sort as the main CheepSync plots.
    """
    has_synced = csv_has_synced(rows)
    n_panels = 3 if has_synced else 1
    n_rows = n_panels * len(devices)
    fig, axes = plt.subplots(
        n_rows,
        1,
        figsize=(10, 2.8 * max(2, n_rows)),
        squeeze=False,
    )
    ax_flat = axes.flatten()

    for di, addr in enumerate(devices):
        block = by_addr[addr]
        name = block[0].get("device_name", "") or addr
        t_us = np.array([int(r["t_us"]) for r in block], dtype=np.int64)
        recv_ns = np.array([int(r["received_at_ns"]) for r in block], dtype=np.int64)
        seq = np.array([int(r["seq"]) for r in block], dtype=np.int64)
        order = np.argsort(recv_ns)
        t_us = t_us[order]
        recv_ns = recv_ns[order]
        seq = seq[order]

        base = di * n_panels
        ax0 = ax_flat[base + 0]

        dt = np.diff(t_us.astype(np.float64))
        idx = np.arange(1, len(t_us))
        dev_us = dt - 1_000_000.0
        ax0.axhline(0.0, color="0.45", ls="--", lw=1)
        ax0.plot(idx, dev_us, "o-", ms=3, color="C0", label="Δt_us − 1e6")
        bad = np.abs(dev_us) > 1
        n_bad = int(np.sum(bad))
        ax0.set_ylabel("µs")
        mono_t = bool(np.all(dt > 0)) if dt.size else True
        ax0.set_title(
            f"{name} — t_us step check (expect 1_000_000 µs per step). "
            f"t_us strictly increasing: {mono_t}; "
            f"steps ≠ 1e6: {n_bad}"
        )
        ax0.set_xlabel("sample index (after sort by received_at_ns)")
        ax0.grid(True, alpha=0.3)
        ax0.legend(loc="best", fontsize=8)

        if not has_synced:
            continue

        synced = parse_synced_ns(block)
        if synced is None:
            continue
        synced = synced[order]
        alpha_l, beta_l = parse_logged_fit(block)
        tb_ns = t_us.astype(np.float64) * 1000.0
        pred_csv = None
        if alpha_l is not None and beta_l is not None:
            alpha_l = alpha_l[order]
            beta_l = beta_l[order]
            pred_csv = alpha_l + beta_l * tb_ns

        ax1 = ax_flat[base + 1]
        d_sr_ms = (synced - recv_ns.astype(np.float64)) / 1_000_000.0
        ax1.axhline(0.0, color="0.45", ls="--", lw=1)
        ax1.plot(t_us, d_sr_ms, ".", ms=5, color="C1", label="synced_at_ns − received_at_ns")
        ds = np.diff(synced)
        sync_mono = bool(np.all(ds > 0)) if ds.size else True
        ax1.set_ylabel("ms")
        ax1.set_title(
            f"synced vs raw receive (ms); synced_at_ns strictly increasing: {sync_mono} "
            f"(y-axis 2–98%ile so one bad row does not flatten the plot)"
        )
        y1a, y1b = _ylim_percentile(d_sr_ms)
        ax1.set_ylim(y1a, y1b)
        ax1.grid(True, alpha=0.3)
        ax1.legend(loc="best", fontsize=8)

        ax2 = ax_flat[base + 2]
        if pred_csv is not None:
            d_sp_ms = (synced - pred_csv) / 1_000_000.0
            ax2.axhline(0.0, color="0.45", ls="--", lw=1)
            ax2.plot(t_us, d_sp_ms, ".", ms=5, color="C2", label="synced − (CSV α+β·t_beacon)")
            dmax = float(np.nanmax(np.abs(d_sp_ms)))
            y2a, y2b = _ylim_percentile(d_sp_ms)
            ax2.set_ylim(y2a, y2b)
            ax2.set_title(
                f"synced vs CheepSync CSV pred (ms); max |Δ| (full data)={dmax:.6f} ms; "
                f"y-axis 2–98%ile (expect ~0 if synced is mapped time)"
            )
        else:
            ax2.text(0.5, 0.5, "No cheep_sync_alpha_ns / cheep_sync_beta — cannot compare synced to pred", ha="center", va="center", transform=ax2.transAxes)
            ax2.set_xticks([])
            ax2.set_yticks([])
        ax2.set_ylabel("ms")
        ax2.set_xlabel("Beacon t_us (µs)")
        ax2.grid(True, alpha=0.3)
        if pred_csv is not None:
            ax2.legend(loc="best", fontsize=8)

    fig.suptitle(
        "Logging QA (separate file when using -o … --logging-checks): t_us steps; "
        "synced_at_ns vs receive & vs CSV α+β (2–98% y-lims on synced panels)",
        fontsize=8.5,
        y=1.01,
    )
    fig.tight_layout()
    if output:
        fig.savefig(output, dpi=150, bbox_inches="tight")
        print(f"Wrote {output}")
    if show:
        plt.show()
    plt.close(fig)


def main() -> None:
    p = argparse.ArgumentParser(
        description="Plot CheepSync regression line from exported ble_sync_*.csv"
    )
    p.add_argument("csv", type=Path, help="Path to exported CSV")
    p.add_argument(
        "--device",
        help="Only plot this device_address (default: one subplot per device)",
    )
    p.add_argument(
        "--window",
        type=int,
        default=DEFAULT_WINDOW,
        help=f"Sliding window size (default {DEFAULT_WINDOW}, matches app)",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Save figure to this path instead of showing interactively",
    )
    p.add_argument(
        "--logging-checks",
        action="store_true",
        help="Also write a logging-QA figure (t_us steps; synced_at_ns vs receive & vs CSV pred)",
    )
    args = p.parse_args()

    rows = load_csv(args.csv)
    if not rows:
        raise SystemExit("CSV is empty")

    by_addr: dict[str, list[dict[str, str]]] = defaultdict(list)
    for r in rows:
        by_addr[r["device_address"]].append(r)

    if args.device:
        if args.device not in by_addr:
            raise SystemExit(f"No rows for device_address={args.device!r}")
        devices = [args.device]
    else:
        devices = sorted(by_addr.keys())

    has_logged = csv_has_logged_fit(rows)
    n_dev = len(devices)
    panels = 3 if has_logged else 2
    n_rows = panels * n_dev
    fig, axes = plt.subplots(
        n_rows,
        1,
        figsize=(10, 3.4 * max(2, n_rows)),
        squeeze=False,
        sharex=False,
    )
    ax_list = axes.flatten()

    for i, addr in enumerate(devices):
        ax_dev = ax_list[panels * i + 0]
        ax_res = ax_list[panels * i + 1]
        ax_cmp = ax_list[panels * i + 2] if has_logged else None
        block = by_addr[addr]
        t_us = np.array([int(r["t_us"]) for r in block], dtype=np.int64)
        recv_ns = np.array([int(r["received_at_ns"]) for r in block], dtype=np.int64)
        order = np.argsort(recv_ns)
        t_us = t_us[order]
        recv_ns = recv_ns[order]

        alpha_logged, beta_logged = parse_logged_fit(block)
        if alpha_logged is not None:
            alpha_logged = alpha_logged[order]
            beta_logged = beta_logged[order]

        alpha_arr, beta_arr, pred_arr, rms_win_arr = cheep_sync_replay(
            t_us, recv_ns, window_size=args.window
        )
        tb_ns = t_us.astype(np.float64) * 1000.0
        r0 = float(recv_ns[0])
        # Hypothetical receiver time if clock rate matched beacon (β=1), same anchor at first sample.
        pred_unity = r0 + (tb_ns - tb_ns[0])
        skew_vs_unity_ms = (pred_arr - pred_unity) / 1_000_000.0
        meas_vs_unity_ms = (recv_ns.astype(np.float64) - pred_unity) / 1_000_000.0

        pred_logged = None
        skew_logged_ms = None
        diff_pred_ms = None
        if alpha_logged is not None and beta_logged is not None:
            pred_logged = alpha_logged + beta_logged * tb_ns
            skew_logged_ms = (pred_logged - pred_unity) / 1_000_000.0
            both = np.isfinite(pred_arr) & np.isfinite(pred_logged)
            diff_pred_ms = np.full_like(pred_arr, np.nan, dtype=np.float64)
            diff_pred_ms[both] = (pred_arr[both] - pred_logged[both]) / 1_000_000.0
        valid = np.isfinite(pred_arr)
        max_skew_ms = float(
            np.nanmax(np.abs(skew_vs_unity_ms)) if np.any(valid) else 0.0
        )

        resid_ms = (recv_ns.astype(np.float64) - pred_arr) / 1_000_000.0
        rms_all_ms = float(np.sqrt(np.nanmean(resid_ms**2)))

        beta_finite = beta_arr[np.isfinite(beta_arr)]
        final_i = int(np.where(valid)[0][-1]) if np.any(valid) else -1
        final_beta = float(beta_arr[final_i]) if final_i >= 0 else float("nan")
        final_alpha = float(alpha_arr[final_i]) if final_i >= 0 else float("nan")
        rms_win_final = float(rms_win_arr[final_i]) if final_i >= 0 else float("nan")
        skew_ppm = (final_beta - 1.0) * 1e6
        beta_range = (
            f"{beta_finite.min():.9f} … {beta_finite.max():.9f}"
            if beta_finite.size
            else "n/a"
        )
        name = block[0].get("device_name", "") or addr

        # CheepSync vs β=1: deviation from unity clock (ms). Unity model = horizontal y=0 (grey dashed).
        tx = t_us.astype(np.float64)
        ax_dev.axhline(
            0.0,
            color="0.45",
            linewidth=2,
            linestyle="--",
            label="β=1 (anchored at 1st sample)",
            zorder=1,
        )
        ax_dev.plot(
            tx[valid],
            skew_vs_unity_ms[valid],
            color="C3",
            linewidth=2.2,
            label="replay: pred − β=1",
            zorder=3,
        )
        if skew_logged_ms is not None:
            vl = np.isfinite(skew_logged_ms)
            ax_dev.plot(
                tx[vl],
                skew_logged_ms[vl],
                color="darkorange",
                linewidth=1.8,
                linestyle=":",
                label="CSV α,β: pred − β=1",
                zorder=4,
            )
        ax_dev.scatter(
            tx,
            meas_vs_unity_ms,
            s=10,
            alpha=0.55,
            color="C0",
            label="measured − β=1",
            zorder=2,
        )
        ax_dev.set_ylabel("deviation from β=1 clock (ms)")
        ax_dev.set_title(
            f"{name} — β at last sample = {final_beta:.9f}  (skew vs unity: {skew_ppm:+.2f} ppm)  |  "
            f"β range over run: {beta_range}\n"
            f"α at last sample = {final_alpha:.6e} ns  |  max |CheepSync−β=1| = {max_skew_ms:.3f} ms  |  "
            f"RMS: last-window (at last sample)={rms_win_final:.4f} ms, "
            f"all vs time-varying fit={rms_all_ms:.4f} ms"
        )
        ax_dev.legend(loc="best", fontsize=8)
        ax_dev.grid(True, alpha=0.3)

        y_parts = [
            meas_vs_unity_ms,
            skew_vs_unity_ms[np.isfinite(skew_vs_unity_ms)],
            [0.0],
        ]
        if skew_logged_ms is not None:
            y_parts.append(skew_logged_ms[np.isfinite(skew_logged_ms)])
        ystack = np.concatenate(y_parts)
        ylo, yhi = np.percentile(ystack, [2, 98])
        pad = max(0.08 * (yhi - ylo), 0.2)
        ax_dev.set_ylim(ylo - pad, yhi + pad)

        ax_res.axhline(0.0, color="0.5", linewidth=1, linestyle="--")
        ax_res.scatter(t_us, resid_ms, s=8, alpha=0.6, color="C2", label="replay α,β", zorder=3)
        resid_logged_ms = None
        ax_res_right = None
        if pred_logged is not None:
            resid_logged_ms = (recv_ns.astype(np.float64) - pred_logged) / 1_000_000.0
            # CSV and replay residuals are identical within float noise (~ns), so a second
            # scatter on the same axes is invisible. Plot their difference on a twin axis.
            diff_resid_ns = (resid_logged_ms - resid_ms) * 1_000_000.0
            ax_res_right = ax_res.twinx()
            ax_res_right.axhline(0.0, color="darkorange", ls=":", alpha=0.6, lw=1)
            ax_res_right.plot(
                t_us,
                diff_resid_ns,
                "+",
                ms=7,
                mew=1.5,
                color="darkorange",
                alpha=0.85,
                label="CSV − replay residual (ns)",
                zorder=4,
            )
            ax_res_right.set_ylabel("CSV − replay (ns)", color="darkorange")
            ax_res_right.tick_params(axis="y", labelcolor="darkorange")
        ax_res.set_ylabel("residual (ms)")
        ax_res.set_xlabel("Beacon t_us (µs)")
        rtitle = (
            "Residual: measured − (αᵢ + βᵢ·t_beacon_ns); replay from t_us+received_at_ns "
            f"(window {args.window})"
        )
        if pred_logged is not None:
            rtitle += (
                " — green=replay; CSV matches replay (see validation below), "
                "right axis = per-point CSV−replay in ns (expect ~0)"
            )
        ax_res.set_title(rtitle)
        h0, l0 = ax_res.get_legend_handles_labels()
        if ax_res_right is not None:
            h1, l1 = ax_res_right.get_legend_handles_labels()
            ax_res.legend(h0 + h1, l0 + l1, loc="best", fontsize=7)
        else:
            ax_res.legend(loc="best", fontsize=7)
        ax_res.grid(True, alpha=0.3)
        # Zoom y to bulk of data (ignore rare outliers for scale)
        if resid_ms.size:
            lo, hi = np.nanpercentile(resid_ms, [5, 95])
            pad = max(0.5 * (hi - lo), 0.05)
            ax_res.set_ylim(lo - pad, hi + pad)

        if ax_cmp is not None and diff_pred_ms is not None:
            ax_cmp.axhline(0.0, color="0.5", linewidth=1, linestyle="--")
            m = np.isfinite(diff_pred_ms)
            ax_cmp.plot(tx[m], diff_pred_ms[m], color="purple", linewidth=1.2, label="replay − CSV")
            ax_cmp.set_ylabel("ms")
            dmax = float(np.nanmax(np.abs(diff_pred_ms))) if np.any(m) else 0.0
            drms = float(np.sqrt(np.nanmean(diff_pred_ms[m] ** 2))) if np.any(m) else 0.0
            ax_cmp.set_title(
                "Validation: replay prediction − CSV (α+β·t_beacon_ns); "
                f"max |Δ|={dmax:.6f} ms, RMS={drms:.6f} ms (expect ~0)"
            )
            ax_cmp.grid(True, alpha=0.3)
            ax_cmp.legend(loc="best", fontsize=8)
            pad_d = max(0.2 * dmax, 1e-6) if dmax > 0 else 0.01
            ax_cmp.set_ylim(-dmax - pad_d, dmax + pad_d)

        ax_dev.sharex(ax_res)
        if ax_cmp is not None:
            ax_cmp.sharex(ax_res)
        ax_dev.tick_params(labelbottom=False)
        if ax_cmp is not None:
            ax_res.tick_params(labelbottom=False)
            ax_cmp.set_xlabel("Beacon t_us (µs)")
            ax_res.set_xlabel("")

    st = (
        f"CheepSync: replay uses last ≤{args.window} samples from t_us + received_at_ns per row. "
    )
    if has_logged:
        st += (
            "Orange/validation: uses cheep_sync_alpha_ns & cheep_sync_beta from CSV when present."
        )
    else:
        st += "Add cheep_sync_alpha_ns & cheep_sync_beta columns to compare with the app export."
    fig.suptitle(st, fontsize=9, y=1.002)
    fig.tight_layout()
    if args.output:
        fig.savefig(args.output, dpi=150, bbox_inches="tight")
        print(f"Wrote {args.output}")
    else:
        plt.show()
    plt.close(fig)

    if args.logging_checks:
        log_out = None
        if args.output:
            log_out = args.output.with_name(
                args.output.stem + "_logging_checks" + args.output.suffix
            )
        plot_logging_checks_figure(
            by_addr,
            devices,
            rows,
            output=log_out,
            show=not args.output,
        )


if __name__ == "__main__":
    main()
