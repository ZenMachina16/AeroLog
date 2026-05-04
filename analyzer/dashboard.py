"""
AeroLog Interactive Dashboard.

Reads a binary .alog file produced by the C++ consumer and generates a
self-contained HTML file with three linked subplots:

  1. Price chart  — bid / ask / last_price over time
  2. Volume bars  — trade volume per event
  3. Latency histogram — distribution of hot-path write latencies (ns)

Usage:
    python3 dashboard.py [path/to/telemetry.alog] [output.html]

Output:
    dashboard.html  (self-contained, no internet required to open)
"""

import os
import struct
import sys
import datetime

try:
    import plotly.graph_objects as go
    from plotly.subplots import make_subplots
except ImportError:
    print("Error: plotly is not installed.")
    print("  Run:  pip install -r requirements.txt")
    sys.exit(1)

# ---------------------------------------------------------------------------
# ABI constants — must match LogEntry in src/shm_shared.hpp
# ---------------------------------------------------------------------------
ENTRY_FMT  = struct.Struct("=Q8sdddIB3xQ")
ENTRY_SIZE = ENTRY_FMT.size   # 56 bytes
assert ENTRY_SIZE == 56

EVENT_NAMES = {0: "TRADE", 1: "QUOTE", 2: "CANCEL"}


# ---------------------------------------------------------------------------
# Binary decoder
# ---------------------------------------------------------------------------
def load_log(path: str) -> list[dict]:
    if not os.path.exists(path):
        print(f"Error: file not found — {path}")
        print("  Did you run the C++ consumer?")
        sys.exit(1)

    entries = []
    with open(path, "rb") as f:
        while True:
            raw = f.read(ENTRY_SIZE)
            if len(raw) < ENTRY_SIZE:
                break
            ts, sym_b, bid, ask, last, vol, etype, lat = ENTRY_FMT.unpack(raw)
            entries.append({
                "ts_ns":    ts,
                "symbol":   sym_b.rstrip(b"\x00").decode("ascii", errors="replace"),
                "bid":      bid,
                "ask":      ask,
                "last":     last,
                "volume":   vol,
                "etype":    EVENT_NAMES.get(etype, "UNK"),
                "lat_ns":   lat,
            })

    print(f"Loaded {len(entries):,} entries from {path}")
    return entries


# ---------------------------------------------------------------------------
# Dashboard builder
# ---------------------------------------------------------------------------
def build_dashboard(entries: list[dict], output_path: str) -> None:
    if not entries:
        print("No data to plot.")
        return

    # Convert nanosecond timestamps to datetime objects
    times = [
        datetime.datetime.fromtimestamp(e["ts_ns"] / 1e9)
        if e["ts_ns"] > 1_000_000_000  # sanity: already ns epoch
        else datetime.datetime.fromtimestamp(e["ts_ns"])
        for e in entries
    ]

    bids    = [e["bid"]    for e in entries]
    asks    = [e["ask"]    for e in entries]
    lasts   = [e["last"]   for e in entries]
    volumes = [e["volume"] for e in entries]
    lats    = [e["lat_ns"] for e in entries if e["lat_ns"] > 0]

    # ── Figure layout: 3 rows ────────────────────────────────────────────────
    fig = make_subplots(
        rows=3, cols=1,
        shared_xaxes=False,
        row_heights=[0.50, 0.20, 0.30],
        vertical_spacing=0.08,
        subplot_titles=(
            "Price  (Bid / Ask / Last)",
            "Volume per Event",
            f"Hot-Path Write Latency Distribution  (n={len(lats):,})",
        ),
    )

    # ── Row 1: Price traces ──────────────────────────────────────────────────
    fig.add_trace(
        go.Scatter(x=times, y=lasts, name="Last Price",
                   line=dict(color="#00e676", width=1.2)),
        row=1, col=1,
    )
    fig.add_trace(
        go.Scatter(x=times, y=bids, name="Bid",
                   line=dict(color="#2979ff", width=0.8, dash="dot"),
                   opacity=0.7),
        row=1, col=1,
    )
    fig.add_trace(
        go.Scatter(x=times, y=asks, name="Ask",
                   line=dict(color="#ff5252", width=0.8, dash="dot"),
                   opacity=0.7),
        row=1, col=1,
    )

    # ── Row 2: Volume bars ───────────────────────────────────────────────────
    fig.add_trace(
        go.Bar(x=times, y=volumes, name="Volume",
               marker_color="#ffd740", opacity=0.8),
        row=2, col=1,
    )

    # ── Row 3: Latency histogram ─────────────────────────────────────────────
    if lats:
        lats_sorted = sorted(lats)
        n = len(lats_sorted)

        def pct(p: float) -> int:
            return lats_sorted[int(p / 100 * n)]

        p50, p99 = pct(50), pct(99)

        fig.add_trace(
            go.Histogram(
                x=lats,
                nbinsx=100,
                name="Write Latency",
                marker_color="#ce93d8",
                opacity=0.85,
            ),
            row=3, col=1,
        )

        # Percentile annotation lines
        for label, val, color in [("P50", p50, "#80cbc4"), ("P99", p99, "#ef9a9a")]:
            fig.add_vline(
                x=val, row=3, col=1,
                line_width=1.5, line_dash="dash", line_color=color,
                annotation_text=f"{label}={val:,} ns",
                annotation_position="top right",
                annotation_font_color=color,
            )

    # ── Global layout ────────────────────────────────────────────────────────
    fig.update_layout(
        title=dict(
            text="AeroLog — High-Frequency Market Data Reconstruction",
            font=dict(size=18),
        ),
        template="plotly_dark",
        height=900,
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="right", x=1),
        margin=dict(l=60, r=40, t=80, b=40),
    )

    fig.update_yaxes(title_text="Price (USD)", row=1, col=1)
    fig.update_yaxes(title_text="Volume",      row=2, col=1)
    fig.update_xaxes(title_text="Time",        row=2, col=1)
    fig.update_xaxes(title_text="Latency (ns)", row=3, col=1)
    fig.update_yaxes(title_text="Frequency",   row=3, col=1)

    # ── Export ───────────────────────────────────────────────────────────────
    fig.write_html(output_path, include_plotlyjs="cdn")
    print(f"\nDashboard saved to: {output_path}")
    print("Open it in any browser — no server required.")

    # Print quick latency summary to terminal
    if lats:
        lats_sorted = sorted(lats)
        n = len(lats_sorted)
        def pct(p):
            return lats_sorted[int(p / 100 * n)]
        print(f"\nLatency summary ({n:,} events):")
        print(f"  P50  : {pct(50):>8,} ns")
        print(f"  P90  : {pct(90):>8,} ns")
        print(f"  P99  : {pct(99):>8,} ns")
        print(f"  P99.9: {pct(99.9):>8,} ns")
        print(f"  Max  : {lats_sorted[-1]:>8,} ns")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    log_path = sys.argv[1] if len(sys.argv) > 1 else "../src/telemetry.alog"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "dashboard.html"

    data = load_log(log_path)
    build_dashboard(data, out_path)
