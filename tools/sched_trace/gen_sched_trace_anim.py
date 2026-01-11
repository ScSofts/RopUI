#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set


LINE_RE = re.compile(r"^SCHED\|(\d+)\|(\d+)\|([^|]*)\|(.*)$")


def parse_kv(payload: str) -> Dict[str, str]:
    payload = payload.strip()
    if not payload:
        return {}
    out: Dict[str, str] = {}
    for part in payload.split():
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        out[k] = v
    return out


@dataclass(frozen=True)
class Event:
    ts_ns: int
    worker_id: int
    name: str
    kv: Dict[str, str]


def iter_events(lines: Iterable[str]) -> Iterable[Event]:
    for line in lines:
        line = line.rstrip("\n")
        pos = line.find("SCHED|")
        if pos == -1:
            continue
        m = LINE_RE.match(line[pos:])
        if not m:
            continue
        ts_ns = int(m.group(1))
        worker_id = int(m.group(2))
        name = m.group(3)
        kv = parse_kv(m.group(4))
        yield Event(ts_ns=ts_ns, worker_id=worker_id, name=name, kv=kv)


def load_events(path: Optional[Path]) -> List[Event]:
    if path is None or str(path) == "-":
        return list(iter_events(sys.stdin))
    return list(iter_events(path.read_text(encoding="utf-8", errors="replace").splitlines()))


def _maybe_int(s: Optional[str]) -> Optional[int]:
    if s is None:
        return None
    if not s.isdigit():
        return None
    return int(s)


def build_dataset(events: List[Event]) -> Dict:
    if not events:
        return {"meta": {"workers": [], "duration_ms": 0}, "events": []}

    events = sorted(events, key=lambda e: e.ts_ns)
    t0 = events[0].ts_ns
    t1 = events[-1].ts_ns
    duration_ms = max(0, (t1 - t0) // 1_000_000)

    workers: Set[int] = set()
    for e in events:
        workers.add(e.worker_id)
        if e.name.startswith("steal_"):
            v = _maybe_int(e.kv.get("victim"))
            if v is not None:
                workers.add(v)
    worker_list = sorted(workers)

    max_local = defaultdict(int)
    for e in events:
        # Prefer the snapshots that already carry `local`.
        for k in ("local", "victim_local"):
            v = _maybe_int(e.kv.get(k))
            if v is None:
                continue
            if k == "local":
                max_local[e.worker_id] = max(max_local[e.worker_id], v)
            else:
                victim = _maybe_int(e.kv.get("victim"))
                if victim is not None:
                    max_local[victim] = max(max_local[victim], v)

    packed_events: List[Dict] = []
    for e in events:
        packed_events.append(
            {
                "t_ms": int((e.ts_ns - t0) // 1_000_000),
                "w": e.worker_id,
                "name": e.name,
                "kv": e.kv,
            }
        )

    return {
        "meta": {
            "t0_ns": t0,
            "t1_ns": t1,
            "duration_ms": int(duration_ms),
            "workers": worker_list,
            "max_local": {str(k): int(v) for k, v in max_local.items()},
        },
        "events": packed_events,
    }


HTML_TEMPLATE = """<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>RopHive Sched Trace Animation</title>
  <style>
    body { font-family: ui-monospace, Menlo, Consolas, monospace; margin: 0; background: #0b0f14; color: #e6edf3; }
    header { padding: 12px 16px; border-bottom: 1px solid #1f2a37; background: #0b0f14; position: sticky; top: 0; }
    .row { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; }
    button, select, input { background: #111827; color: #e6edf3; border: 1px solid #2a3b52; border-radius: 6px; padding: 6px 10px; }
    input[type="range"] { width: min(720px, 90vw); }
    #wrap { padding: 12px 16px; }
    canvas { width: 100%; border: 1px solid #1f2a37; border-radius: 10px; background: #06080c; }
    #cvTop { height: 420px; }
    #cvBottom { height: 380px; margin-top: 12px; }
    .muted { color: #9aa4b2; }
    .kv { white-space: pre; }
  </style>
</head>
<body>
  <header>
    <div class="row">
      <button id="btnPlay">Play</button>
      <button id="btnStep">Step</button>
      <span class="muted">t=</span><span id="tLabel">0ms</span>
      <input id="seek" type="range" min="0" max="0" value="0" step="1" />
      <span class="muted">playback</span>
      <select id="speed">
        <option value="50">1s→50ms</option>
        <option value="100">1s→100ms</option>
        <option value="200" selected>1s→200ms</option>
        <option value="500">1s→500ms</option>
        <option value="1000">1s→1000ms</option>
        <option value="2000">1s→2000ms</option>
      </select>
      <label class="muted"><input id="chkFollow" type="checkbox" checked />follow</label>
    </div>
    <div class="row muted" style="margin-top:8px">
      <span id="metaLabel"></span>
      <span id="eventLabel"></span>
    </div>
  </header>
  <div id="wrap">
    <canvas id="cvTop" width="1600" height="420"></canvas>
    <canvas id="cvBottom" width="1600" height="380"></canvas>
    <div class="muted" style="margin-top:10px">
      下方面板：左侧 steal 热力图（行=thief，列=victim，滑动窗口），右侧 recent steals。
    </div>
    <div id="lastEvent" class="kv muted" style="margin-top:8px"></div>
  </div>

  <script id="data" type="application/json"></script>
  <script>
  (function() {
    const dataset = JSON.parse(document.getElementById("data").textContent);
    const meta = dataset.meta || {};
    const events = dataset.events || [];
    const workers = meta.workers || [];
    const durationMs = meta.duration_ms || 0;
    const maxLocal = meta.max_local || {};

    const cvTop = document.getElementById("cvTop");
    const cvBottom = document.getElementById("cvBottom");
    const ctxTop = cvTop.getContext("2d");
    const ctxBottom = cvBottom.getContext("2d");

    const btnPlay = document.getElementById("btnPlay");
    const btnStep = document.getElementById("btnStep");
    const seek = document.getElementById("seek");
    const speedSel = document.getElementById("speed");
    const chkFollow = document.getElementById("chkFollow");
    const tLabel = document.getElementById("tLabel");
    const metaLabel = document.getElementById("metaLabel");
    const eventLabel = document.getElementById("eventLabel");
    const lastEvent = document.getElementById("lastEvent");

    metaLabel.textContent = `workers=${workers.length} duration=${durationMs}ms`;

    seek.max = String(durationMs);
    seek.value = "0";

    function colorFor(state) {
      switch (state) {
        case "sleep": return "#2b6cb0";
        case "blocked": return "#6b7280";
        case "local": return "#2f855a";
        case "global": return "#0ea5b7";
        case "timer": return "#d69e2e";
        case "private": return "#805ad5";
        case "steal": return "#e53e3e";
        default: return "#374151";
      }
    }

    function stateFromEventName(name) {
      if (name === "sleep_enter") return "sleep";
      if (name === "sleep_exit") return "idle";
      if (name === "user_block_begin") return "blocked";
      if (name === "user_block_end") return "idle";
      if (name === "local_done" || name === "local_push") return "local";
      if (name === "global_harvest_done") return "global";
      if (name === "timer_done") return "timer";
      if (name === "private_done") return "private";
      if (name.startsWith("steal_")) return "steal";
      return null;
    }

    // Per-worker state + last known queue snapshot.
    function makeState() {
      const st = {};
      for (const w of workers) {
        st[w] = {
          mode: "idle",
          until: 0,
          local: 0,
          inbound: 0,
          global: 0,
          timers: 0,
          private: 0,
          lastEvent: null,
        };
      }
      return st;
    }

    function applyKvSnapshot(wst, kv) {
      if (!kv) return;
      for (const k of ["local", "inbound", "global", "timers", "private"]) {
        if (kv[k] !== undefined) {
          const v = Number(kv[k]);
          if (!Number.isNaN(v)) wst[k] = v;
        }
      }
      // If we observe pending local work, avoid showing "idle" just because
      // events are sparse.
      if (wst.mode === "idle" && (wst.local || 0) > 0) {
        wst.mode = "local";
        wst.until = Math.max(wst.until, tMs + 80);
      }
    }

    let tMs = 0;
    let playing = false;
    let lastTs = performance.now();
    let eventIndex = 0;
    let appliedCount = 0;
    // Sliding-window steal stats for readability (instead of arrows).
    const stealWindowMs = 200;
    let stealQueue = []; // {t_ms, thief, victim, stolen}
    let stealMatrix = []; // [thiefIndex][victimIndex] -> stolen_tasks in window
    let stealMax = 1;
    let recentSteals = []; // recent textual list
    let st = makeState();

    function resetTo(timeMs) {
      tMs = timeMs;
      eventIndex = 0;
      appliedCount = 0;
      stealQueue = [];
      stealMatrix = workers.map(() => workers.map(() => 0));
      stealMax = 1;
      recentSteals = [];
      st = makeState();
      // Apply all events up to tMs.
      while (eventIndex < events.length && events[eventIndex].t_ms <= tMs) {
        applyEvent(events[eventIndex]);
        eventIndex += 1;
      }
    }

    function applyEvent(ev) {
      appliedCount += 1;
      const wst = st[ev.w];
      if (!wst) return;
      wst.lastEvent = ev;

      if (ev.name === "runonce_begin" && ev.kv) {
        applyKvSnapshot(wst, ev.kv);
      }
      if (ev.name === "local_done" && ev.kv && ev.kv.local !== undefined) {
        applyKvSnapshot(wst, ev.kv);
      }
      if (ev.name === "global_harvest_done" && ev.kv && ev.kv.local !== undefined) {
        applyKvSnapshot(wst, ev.kv);
      }
      if (ev.name === "steal_success" && ev.kv) {
        // Snapshots: thief local / victim local.
        if (ev.kv.local !== undefined) {
          const v = Number(ev.kv.local);
          if (!Number.isNaN(v)) wst.local = v;
        }
        if (ev.kv.victim !== undefined && ev.kv.victim_local !== undefined) {
          const victim = Number(ev.kv.victim);
          const vlocal = Number(ev.kv.victim_local);
          if (!Number.isNaN(victim) && st[victim] && !Number.isNaN(vlocal)) {
            st[victim].local = vlocal;
          }
        }
      }

      const state = stateFromEventName(ev.name);
      if (state === "idle") {
        wst.mode = "idle";
        wst.until = ev.t_ms;
      } else if (state === "sleep") {
        wst.mode = "sleep";
        wst.until = 1e18;
      } else if (state === "blocked") {
        wst.mode = "blocked";
        wst.until = 1e18;
      } else if (state) {
        wst.mode = state;
        wst.until = ev.t_ms + 60;
      }

      if (ev.name === "steal_success") {
        // Make the thief visibly transition into running local work after a steal.
        wst.mode = "local";
        wst.until = ev.t_ms + 180;

        const victim = ev.kv && ev.kv.victim ? Number(ev.kv.victim) : NaN;
        const stolen = ev.kv && ev.kv.stolen ? Number(ev.kv.stolen) : 0;
        if (!Number.isNaN(victim)) {
          const thief = ev.w;
          stealQueue.push({ t_ms: ev.t_ms, thief, victim, stolen });
          const ti = workers.indexOf(thief);
          const vi = workers.indexOf(victim);
          if (ti >= 0 && vi >= 0) {
            stealMatrix[ti][vi] += (Number.isFinite(stolen) ? stolen : 0);
            stealMax = Math.max(stealMax, stealMatrix[ti][vi] || 0);
          }
          recentSteals.unshift({ t_ms: ev.t_ms, thief, victim, stolen });
          if (recentSteals.length > 10) recentSteals.length = 10;
        }
      }

      if (ev.name === "worker_run_stop") {
        wst.mode = "idle";
        wst.until = ev.t_ms;
      }
    }

    function tickApply() {
      while (eventIndex < events.length && events[eventIndex].t_ms <= tMs) {
        applyEvent(events[eventIndex]);
        eventIndex += 1;
      }
      // Expire flash states
      for (const w of workers) {
        const wst = st[w];
        if (wst.mode !== "sleep" && wst.mode !== "blocked" && wst.mode !== "idle" && tMs >= wst.until) {
          wst.mode = "idle";
        }
        if (wst.mode === "idle" && (wst.local || 0) > 0) {
          wst.mode = "local";
        }
      }
      // expire steal window
      const cutoff = tMs - stealWindowMs;
      while (stealQueue.length && stealQueue[0].t_ms < cutoff) {
        const s = stealQueue.shift();
        const ti = workers.indexOf(s.thief);
        const vi = workers.indexOf(s.victim);
        if (ti >= 0 && vi >= 0) {
          stealMatrix[ti][vi] -= (Number.isFinite(s.stolen) ? s.stolen : 0);
          if (stealMatrix[ti][vi] < 0) stealMatrix[ti][vi] = 0;
        }
      }
      // recompute max occasionally (window is small; N is tiny)
      stealMax = 1;
      for (let i = 0; i < workers.length; i++) {
        for (let j = 0; j < workers.length; j++) {
          stealMax = Math.max(stealMax, stealMatrix[i][j] || 0);
        }
      }
    }

    function resizeIfNeeded() {
      const ratio = window.devicePixelRatio || 1;
      const rectTop = cvTop.getBoundingClientRect();
      const wantW = Math.max(900, Math.floor(rectTop.width * ratio));
      const wantHTop = Math.floor(rectTop.height * ratio);
      const rectBottom = cvBottom.getBoundingClientRect();
      const wantHBottom = Math.floor(rectBottom.height * ratio);

      if (cvTop.width !== wantW || cvTop.height !== wantHTop) {
        cvTop.width = wantW;
        cvTop.height = wantHTop;
      }
      if (cvBottom.width !== wantW || cvBottom.height !== wantHBottom) {
        cvBottom.width = wantW;
        cvBottom.height = wantHBottom;
      }
    }

    function renderTop() {
      resizeIfNeeded();
      const W = cvTop.width, H = cvTop.height;
      const ctx = ctxTop;
      ctx.clearRect(0, 0, W, H);

      const padL = 110, padR = 18, padT = 18, padB = 18;
      const timelineH = 40;
      const rowH = Math.max(15, Math.floor((H - padT - padB - timelineH) / Math.max(1, workers.length)));
      const laneW = W - padL - padR;
      const lanesBottom = padT + workers.length * rowH;
      const timeY = lanesBottom + 16;

      ctx.font = `${Math.max(16, Math.floor(H / 40))}px ui-monospace, Menlo, Consolas, monospace`;

      // title
      const hudY = 20;
      ctx.fillStyle = "rgba(6,8,12,0.85)";
      ctx.fillRect(0, 0, Math.min(W, 520), 34);
      ctx.fillStyle = "#9aa4b2";
      ctx.fillText(`t=${Math.floor(tMs)}ms  applied=${appliedCount}/${events.length}`, 12, hudY);

      // time axis
      ctx.strokeStyle = "#1f2a37";
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(padL, timeY);
      ctx.lineTo(padL + laneW, timeY);
      ctx.stroke();
      ctx.fillStyle = "#9aa4b2";
      ctx.fillText("0ms", padL, timeY + 18);
      ctx.fillText(`${durationMs}ms`, padL + laneW - 88, timeY + 18);
      const cursorX = padL + (laneW * (tMs / Math.max(1, durationMs)));
      ctx.strokeStyle = "rgba(148,163,184,0.65)";
      ctx.beginPath();
      ctx.moveTo(cursorX, padT + 34);
      ctx.lineTo(cursorX, timeY);
      ctx.stroke();

      // lanes
      for (let i = 0; i < workers.length; i++) {
        const wid = workers[i];
        const y = padT + i * rowH;
        const wst = st[wid];
        const mode = wst ? wst.mode : "idle";

        // label
        ctx.fillStyle = "#e6edf3";
        ctx.fillText(`w${wid}`, 16, y + 24);

        // background lane (thinner)
        ctx.fillStyle = "#0b1220";
        ctx.fillRect(padL, y + 6, laneW, rowH - 10);

        // state overlay
        const c = colorFor(mode);
        ctx.fillStyle = c;
        ctx.globalAlpha = (mode === "idle") ? 0.25 : 0.65;
        ctx.fillRect(padL, y + 6, laneW, rowH - 10);
        ctx.globalAlpha = 1.0;

        // local bar
        const maxL = Number(maxLocal[String(wid)] || 0);
        const local = wst ? Number(wst.local || 0) : 0;
        const frac = (maxL > 0) ? Math.min(1, local / maxL) : 0;
        ctx.fillStyle = "#22c55e";
        ctx.globalAlpha = 0.35;
        ctx.fillRect(padL, y + rowH - 14, Math.floor(laneW * frac), 5);
        ctx.globalAlpha = 1.0;

        // stats
        if (wst) {
          ctx.fillStyle = "#cbd5e1";
          const stats = `mode=${mode} local=${wst.local} inbound=${wst.inbound} global=${wst.global} timers=${wst.timers}`;
          ctx.fillText(stats, padL + 10, y + 24);
        }
      }

      // Legend: place to the right of w0 lane (or first lane if w0 missing).
      const w0Index = workers.indexOf(0);
      const legendRow = (w0Index >= 0) ? w0Index : 0;
      const legendX = padL + Math.floor(laneW * 0.70);
      const legendTop = padT + legendRow * rowH + 6;
      const box = 10;
      const items = [
        ["sleep", "#2b6cb0"],
        ["blocked", "#6b7280"],
        ["local", "#2f855a"],
        ["global", "#0ea5b7"],
        ["timer", "#d69e2e"],
        ["private", "#805ad5"],
        ["steal", "#e53e3e"],
      ];
      ctx.fillStyle = "rgba(6,8,12,0.55)";
      const legendW = Math.min(220, padL + laneW - legendX - 10);
      const lineH = 20;
      const legendH = items.length * lineH + 10;
      ctx.fillRect(legendX - 10, legendTop - 6, legendW, legendH);
      for (let i = 0; i < items.length; i++) {
        const [name, color] = items[i];
        const yy = legendTop + i * lineH;
        ctx.fillStyle = color;
        ctx.fillRect(legendX, yy - 10, box, box);
        ctx.fillStyle = "#cbd5e1";
        ctx.fillText(name, legendX + box + 8, yy);
      }

    }

    function renderBottom() {
      resizeIfNeeded();
      const W = cvBottom.width, H = cvBottom.height;
      const ctx = ctxBottom;
      ctx.clearRect(0, 0, W, H);

      const padL = 18, padR = 18, padT = 18, padB = 18;
      const panelW = W - padL - padR;
      const panelH = H - padT - padB;
      const panelX = padL;
      const panelY = padT;

      ctx.font = `${Math.max(16, Math.floor(H / 22))}px ui-monospace, Menlo, Consolas, monospace`;

      ctx.fillStyle = "rgba(6,8,12,0.55)";
      ctx.fillRect(panelX, panelY, panelW, panelH);
      ctx.strokeStyle = "#1f2a37";
      ctx.strokeRect(panelX, panelY, panelW, panelH);

      const gap = 16;
      const leftW = Math.floor(panelW * 0.62);
      const rightW = panelW - leftW - gap;
      const leftX = panelX + 12;
      const rightX = panelX + leftW + gap;
      const topY = panelY + 18;

      ctx.fillStyle = "#9aa4b2";
      ctx.fillText(`steal heatmap (window=${stealWindowMs}ms)`, leftX, topY);
      ctx.fillText("recent steals", rightX, topY);

      const maxCells = Math.max(1, workers.length + 1);
      // Choose cell size so that the (N+1)x(N+1) grid fits in the left panel
      // both horizontally and vertically (important when N is large, e.g. 10+).
      const hmPadX = 12;
      const hmPadY = 12;
      const hmTitleH = 24;
      const hmAvailW = Math.max(10, leftW - 40 - hmPadX);
      const hmAvailH = Math.max(10, panelH - (topY - panelY) - hmTitleH - 40 - hmPadY);
      const cellByW = Math.floor(hmAvailW / maxCells);
      const cellByH = Math.floor(hmAvailH / maxCells);
      const cell = Math.max(16, Math.min(62, Math.min(cellByW, cellByH)));
      const hmX = leftX;
      const hmY = topY + 18;

      const prevFont = ctx.font;
      ctx.font = `${Math.max(22, Math.floor(cell * 0.5))}px ui-monospace, Menlo, Consolas, monospace`;

      for (let j = 0; j < workers.length; j++) {
        ctx.fillStyle = "#cbd5e1";
        ctx.fillText(`v${workers[j]}`, hmX + (j + 1) * cell + 4, hmY + 18);
      }
      for (let i = 0; i < workers.length; i++) {
        ctx.fillStyle = "#cbd5e1";
        ctx.fillText(`t${workers[i]}`, hmX + 4, hmY + (i + 1) * cell + 18);
      }

      for (let i = 0; i < workers.length; i++) {
        for (let j = 0; j < workers.length; j++) {
          const v = stealMatrix[i][j] || 0;
          const a = Math.min(1, v / Math.max(1, stealMax));
          const x = hmX + (j + 1) * cell;
          const y = hmY + (i + 1) * cell;
          ctx.fillStyle = `rgba(239,68,68,${0.08 + 0.75 * a})`;
          ctx.fillRect(x, y, cell - 3, cell - 3);
          if (v > 0) {
            ctx.fillStyle = "rgba(6,8,12,0.85)";
            ctx.fillText(String(v), x + 4, y + Math.floor(cell * 0.7));
          }
        }
      }

      ctx.font = prevFont;
      ctx.fillStyle = "#cbd5e1";
      for (let i = 0; i < recentSteals.length; i++) {
        const s = recentSteals[i];
        const line = `t=${Math.max(0, Math.floor(tMs - s.t_ms))}ms  v${s.victim}→t${s.thief} +${s.stolen}`;
        ctx.fillText(line, rightX, topY + 32 + i * 20);
      }
    }

    function loop() {
      const now = performance.now();
      const dt = now - lastTs;
      lastTs = now;

      if (playing) {
        // speedSel is "how many trace-ms to advance per 1s real time".
        const msPerSecond = Number(speedSel.value || "200");
        const advance = (dt * msPerSecond) / 1000.0;
        tMs = Math.min(durationMs, Math.max(0, tMs + advance));
        if (chkFollow.checked) seek.value = String(Math.floor(tMs));
        tickApply();
        if (tMs >= durationMs) {
          playing = false;
          btnPlay.textContent = "Play";
        }
      }

      tLabel.textContent = `${Math.floor(tMs)}ms`;
      eventLabel.textContent = `applied=${appliedCount}/${events.length}`;

      // last event preview (current worker 0 if exists, else any)
      const w0 = workers.length ? workers[0] : null;
      if (w0 !== null && st[w0] && st[w0].lastEvent) {
        const ev = st[w0].lastEvent;
        lastEvent.textContent = `last(w${w0}): ${ev.name}  kv=${JSON.stringify(ev.kv)}`;
      }

      renderTop();
      renderBottom();
      requestAnimationFrame(loop);
    }

    btnPlay.addEventListener("click", () => {
      playing = !playing;
      btnPlay.textContent = playing ? "Pause" : "Play";
      lastTs = performance.now();
    });

    btnStep.addEventListener("click", () => {
      playing = false;
      btnPlay.textContent = "Play";
      tMs = Math.min(durationMs, tMs + 10);
      seek.value = String(Math.floor(tMs));
      tickApply();
    });

    seek.addEventListener("input", () => {
      const v = Number(seek.value || "0");
      resetTo(v);
    });

    resetTo(0);
    requestAnimationFrame(loop);
  })();
  </script>
</body>
</html>
"""


def write_html(dataset: Dict, out_path: Path) -> None:
    data_json = json.dumps(dataset, ensure_ascii=False, separators=(",", ":"))
    html = HTML_TEMPLATE.replace(
        '<script id="data" type="application/json"></script>',
        f'<script id="data" type="application/json">{data_json}</script>',
    )
    out_path.write_text(html, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate an offline HTML animation for RopHive SCHED trace logs.")
    ap.add_argument("input", nargs="?", default="-", help="log file path or '-' for stdin")
    ap.add_argument("-o", "--out", required=True, help="output html path")
    args = ap.parse_args()

    events = load_events(None if args.input == "-" else Path(args.input))
    dataset = build_dataset(events)
    out = Path(args.out)
    write_html(dataset, out)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
