"""Bounded, streaming analysis of a free-play runtime-timeline.csv.

Usage: python pc-port/scripts/analyze_runtime_timeline.py RUN_DIR
No game process is launched. CPU and GPU clocks are deliberately kept separate.
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, OrderedDict
from pathlib import Path


LIMITS = {"guest_submits": 8192, "sessions": 8192, "flips": 4096, "frames": 4096}
# Fixed buckets make quantiles approximate; each quantile is reported as its bucket upper bound.
BUCKET_MS = (0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 33, 50, 100, 250, 500, 1000)


def number(value: str | None) -> int:
    try:
        return int(value or "0", 0)
    except (ValueError, TypeError):
        return 0


class Metric:
    def __init__(self) -> None:
        self.count = 0
        self.total_ns = 0
        self.max_ns = 0
        self.buckets = [0] * (len(BUCKET_MS) + 1)

    def add(self, value_ns: int) -> None:
        if value_ns < 0:
            return
        self.count += 1
        self.total_ns += value_ns
        self.max_ns = max(self.max_ns, value_ns)
        ms = value_ns / 1_000_000
        for i, bound in enumerate(BUCKET_MS):
            if ms <= bound:
                self.buckets[i] += 1
                break
        else:
            self.buckets[-1] += 1

    def as_json(self) -> dict:
        if not self.count:
            return {"count": 0}

        def quantile_upper(percent: float) -> str:
            target = max(1, int(self.count * percent + 0.999999))
            seen = 0
            for i, count in enumerate(self.buckets):
                seen += count
                if seen >= target:
                    return f"<={BUCKET_MS[i]} ms" if i < len(BUCKET_MS) else ">1000 ms"
            return ">1000 ms"

        return {
            "count": self.count,
            "mean_ms": round(self.total_ns / self.count / 1_000_000, 4),
            "max_ms": round(self.max_ns / 1_000_000, 4),
            "p50_bucket": quantile_upper(0.50),
            "p95_bucket": quantile_upper(0.95),
            "histogram_upper_ms": {str(bound): self.buckets[i] for i, bound in enumerate(BUCKET_MS)},
            "histogram_over_1000_ms": self.buckets[-1],
        }


class Analyzer:
    def __init__(self) -> None:
        self.counts: Counter[str] = Counter()
        self.metrics: dict[str, Metric] = {}
        self.pending: dict[str, OrderedDict] = {name: OrderedDict() for name in LIMITS}
        self.evictions: Counter[str] = Counter()
        self.invalid: Counter[str] = Counter()
        self.health = {"samples": 0, "max_dropped": 0, "last_reported_rows": 0,
                       "last_reported_bytes": 0, "truncated": False}
        self.markers: list[dict] = []
        self.marker_omitted = 0
        self.last_present: dict | None = None
        self.gpu: dict[int, dict] = {}
        self.completion_watermarks: dict[int, dict] = {}
        self.join_counts: Counter[str] = Counter()
        self.first_cpu_ns: int | None = None
        self.last_cpu_ns: int | None = None
        self.rows = 0

    def metric(self, name: str, value_ns: int) -> None:
        if value_ns < 0:
            self.invalid[f"negative_{name}"] += 1
            return
        self.metrics.setdefault(name, Metric()).add(value_ns)

    def state(self, kind: str, key) -> dict:
        table = self.pending[kind]
        if key not in table:
            table[key] = {"_done": set()}
            if len(table) > LIMITS[kind]:
                table.popitem(last=False)
                self.evictions[kind] += 1
        else:
            table.move_to_end(key)
        return table[key]

    def pair(self, state: dict, earlier: str, later: str, metric_name: str) -> None:
        if metric_name in state["_done"] or earlier not in state or later not in state:
            return
        self.metric(metric_name, state[later] - state[earlier])
        state["_done"].add(metric_name)
        self.join_counts[metric_name] += 1

    def event(self, row: dict[str, str]) -> None:
        self.rows += 1
        event = row.get("event", "")
        self.counts[event] += 1
        ns = number(row.get("cpu_ns"))
        if self.first_cpu_ns is None:
            self.first_cpu_ns = ns
        self.last_cpu_ns = ns
        owner, tick = number(row.get("owner")), number(row.get("tick"))
        a, b, c, d = (number(row.get(k)) for k in ("a", "b", "c", "d"))
        duration = number(row.get("duration_ns"))
        if duration:
            self.metric(f"event_duration.{event}", duration)

        if event == "trace_health":
            self.health["samples"] += 1
            self.health["last_reported_rows"] = a
            self.health["last_reported_bytes"] = b
            self.health["max_dropped"] = max(self.health["max_dropped"], c)
        elif event == "trace_truncated":
            self.health["truncated"] = True
            self.health["max_dropped"] = max(self.health["max_dropped"], c)

        if event in ("guest_queue_submit", "cp_gfx_begin", "cp_asc_begin", "cp_complete"):
            # tick=sequence, a=qid, b=generation; owner is the Liverpool instance.
            s = self.state("guest_submits", (owner, a, b, tick))
            s[event] = ns
            if event == "cp_complete" and c:
                s["coroutine_handle"] = c
            self.pair(s, "guest_queue_submit", "cp_gfx_begin", "guest_queue_to_cp_gfx")
            self.pair(s, "guest_queue_submit", "cp_asc_begin", "guest_queue_to_cp_asc")
            self.pair(s, "cp_gfx_begin", "cp_complete", "cp_gfx_begin_to_complete")
            self.pair(s, "cp_asc_begin", "cp_complete", "cp_asc_begin_to_complete")
            self.pair(s, "guest_queue_submit", "cp_complete", "guest_queue_to_cp_complete")

        if event == "completion_observed":
            # The observed tick is cumulative. It may cover several sessions and is
            # only an upper bound on when the GPU actually completed each one.
            previous = self.completion_watermarks.get(owner, {}).get("known_tick", 0)
            if tick < previous:
                self.invalid["completion_watermark_regressed"] += 1
            self.completion_watermarks[owner] = {"known_tick": tick, "cpu_observed_ns": ns,
                                                  "previous_reported_tick": a}

        if event in ("session_enqueued", "recorder_submit_begin", "recorder_submit_end",
                     "vk_queue_submit_begin", "vk_queue_submit_end", "gpu_session_valid",
                     "gpu_session_invalid", "session_open"):
            s = self.state("sessions", (owner, tick))
            s[event] = ns
            if event == "session_enqueued":
                s["path"] = a  # 0 direct, 1 direct after stream, 2 recorder job
            if event == "gpu_session_valid":
                self.gpu_session(owner, tick, a, b, c, d)
            elif event == "gpu_session_invalid":
                self.invalid[f"gpu_reason_{a}"] += 1
            elif event == "vk_queue_submit_end" and a == 0:
                self.invalid["vk_queue_submit_failed"] += 1
            self.pair(s, "session_enqueued", "vk_queue_submit_begin",
                      "session_enqueued_to_vk_submit_begin")
            self.pair(s, "session_enqueued", "recorder_submit_begin",
                      "session_enqueued_to_recorder_begin")
            self.pair(s, "recorder_submit_begin", "vk_queue_submit_begin",
                      "recorder_begin_to_vk_submit_begin")
            self.pair(s, "vk_queue_submit_begin", "vk_queue_submit_end",
                      "vk_queue_submit_wall")
            # Query collection can be delayed after completion; this is an upper
            # bound on latency to observation, not actual GPU completion time.
            self.pair(s, "vk_queue_submit_end", "gpu_session_valid",
                      "vk_submit_end_to_gpu_query_collection_upper_bound")

        if event.startswith("vo_") and owner:
            s = self.state("flips", owner)
            s[event] = ns
            if event == "vo_prepare_end":
                frame_key = (a, b)  # Frame* and ready_tick; pointer alone is reused.
                s["frame_key"] = frame_key
                f = self.state("frames", frame_key)
                f["flip_id"] = owner
                if "present_begin" in f and "present_end" in f and \
                        "present_frame_join" not in f["_done"]:
                    self.join_counts["present_frame_join"] += 1
                    f["_done"].add("present_frame_join")
            if event == "vo_queue_pop":
                s["vblank_count"] = tick
            self.pair(s, "vo_flip_accept", "vo_prepare_begin", "flip_accept_to_prepare_begin")
            self.pair(s, "vo_prepare_begin", "vo_prepare_end", "flip_prepare")
            self.pair(s, "vo_flip_accept", "vo_queue_push", "flip_accept_to_queue_push")
            self.pair(s, "vo_queue_push", "vo_queue_pop", "flip_queue_wait")
            self.pair(s, "vo_queue_pop", "vo_present_return", "flip_pop_to_present_return")
            self.pair(s, "vo_flip_accept", "vo_present_return", "flip_accept_to_present_return")
            self.pair(s, "vo_present_return", "vo_flip_events_labels_done",
                      "flip_present_return_to_guest_events_labels")
            if event == "vo_present_return":
                self.last_present = {"flip_request_id": owner, "cpu_ns": ns,
                                     "observed_guest_frame": number(row.get("observed_guest_frame"))}
            if event == "vo_vblank":
                self.pair(s, "vo_queue_pop", "vo_vblank", "flip_pop_to_vblank")

        if event in ("present_begin", "present_acquired", "present_flushed", "present_end"):
            f = self.state("frames", (owner, tick))
            f[event] = ns
            self.pair(f, "present_begin", "present_acquired", "present_acquire")
            self.pair(f, "present_acquired", "present_flushed", "present_record_and_flush")
            self.pair(f, "present_begin", "present_end", "present_total")
            if "flip_id" in f and "present_begin" in f and "present_end" in f:
                if "present_frame_join" not in f["_done"]:
                    self.join_counts["present_frame_join"] += 1
                    f["_done"].add("present_frame_join")

        if event == "user_stutter_mark":
            mark = {"cpu_ns": ns, "sdl_tick_ms": a,
                    "observed_guest_frame": number(row.get("observed_guest_frame")),
                    "nearby_last_present": self.last_present}
            if len(self.markers) < 1000:
                self.markers.append(mark)
            else:
                self.marker_omitted += 1

    def gpu_session(self, owner: int, tick: int, start: int, end: int,
                    period_ps: int, valid_mask: int) -> None:
        # Raw GPU clock only. No subtraction from any CPU timestamp.
        if not period_ps or end < start:
            self.invalid["gpu_timestamp_wrap_or_period"] += 1
            return
        duration_ns = (end - start) * period_ps // 1000
        self.metric("gpu_session_duration", duration_ns)
        lane = self.gpu.setdefault(owner, {"sessions": 0, "union_ns": 0, "gap_ns": 0,
                                           "overlap_ns": 0, "clock_discontinuities": 0,
                                           "previous_start": None, "previous_end": None,
                                           "last_tick": None, "valid_mask": valid_mask})
        lane["sessions"] += 1
        if lane["previous_start"] is not None:
            if start < lane["previous_start"] or end < lane["previous_end"] or \
                    (lane["last_tick"] is not None and tick <= lane["last_tick"]):
                lane["clock_discontinuities"] += 1
                lane["previous_start"] = None
                lane["previous_end"] = None
        if lane["previous_end"] is None:
            lane["union_ns"] += duration_ns
        else:
            lane["gap_ns"] += max(0, start - lane["previous_end"]) * period_ps // 1000
            lane["overlap_ns"] += max(0, min(end, lane["previous_end"]) - start) * period_ps // 1000
            lane["union_ns"] += max(0, end - max(start, lane["previous_end"])) * period_ps // 1000
        lane["previous_start"] = start
        lane["previous_end"] = max(end, lane["previous_end"] or end)
        lane["last_tick"] = tick

    def summary(self, source: str) -> dict:
        missing = {
            "guest_submits_without_cp_begin": sum(
                "guest_queue_submit" in x and "cp_gfx_begin" not in x and "cp_asc_begin" not in x
                for x in self.pending["guest_submits"].values()),
            "guest_submits_without_complete": sum(
                "guest_queue_submit" in x and "cp_complete" not in x
                for x in self.pending["guest_submits"].values()),
            "sessions_enqueued_without_vk_submit_begin": sum(
                "session_enqueued" in x and "vk_queue_submit_begin" not in x
                for x in self.pending["sessions"].values()),
            "flips_accepted_without_pop": sum(
                "vo_flip_accept" in x and "vo_queue_pop" not in x
                for x in self.pending["flips"].values()),
            "flips_popped_without_present_return": sum(
                "vo_queue_pop" in x and "vo_present_return" not in x
                for x in self.pending["flips"].values()),
        }
        gpu = {}
        for owner, lane in self.gpu.items():
            gpu[str(owner)] = {key: value for key, value in lane.items()
                               if key not in ("previous_start", "previous_end", "last_tick")}
        return {
            "source": source,
            "rows_read": self.rows,
            "cpu_ns_span": [self.first_cpu_ns, self.last_cpu_ns],
            "event_counts": dict(self.counts),
            "trace_health": self.health,
            "invalid_or_skipped": dict(self.invalid),
            "pending_evictions": dict(self.evictions),
            "pending_limits": LIMITS,
            "incomplete_in_retained_maps": missing,
            "joins_completed": dict(self.join_counts),
            "latencies": {key: value.as_json() for key, value in sorted(self.metrics.items())},
            "gpu_raw_by_scheduler": gpu,
            "completion_observed_watermarks": {str(k): v for k, v in self.completion_watermarks.items()},
            "user_stutter_marks": self.markers,
            "user_stutter_marks_omitted": self.marker_omitted,
            "interpretation": [
                "CPU latency joins require matching IDs; observed_guest_frame is not ownership.",
                "GPU raw duration/union/gaps are within each scheduler clock only; no CPU/GPU clock subtraction.",
                "completion_observed is a cumulative watermark; query collection delay is only an upper bound.",
                "A session may contain commands from multiple guest submissions or frames.",
                "Quantiles are approximate fixed-histogram bucket upper bounds.",
                "Missing counts exclude evicted pending entries; inspect pending_evictions and dropped rows.",
                "A nearby F10 mark does not establish causal ownership of the last present.",
            ],
        }


def analyze(run_dir: Path) -> dict:
    source = run_dir / "runtime-timeline.csv"
    if not source.is_file():
        raise FileNotFoundError(source)
    analyzer = Analyzer()
    with source.open(newline="", encoding="utf-8", errors="replace") as stream:
        reader = csv.DictReader(stream)
        expected = {"event", "cpu_ns", "observed_guest_frame", "thread_id", "owner",
                    "tick", "a", "b", "c", "d", "duration_ns"}
        if not expected.issubset(reader.fieldnames or []):
            raise ValueError(f"runtime timeline missing columns: {sorted(expected - set(reader.fieldnames or []))}")
        for row in reader:
            analyzer.event(row)
    result = analyzer.summary(str(source.resolve()))
    output = run_dir / "timeline-summary.json"
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args()
    result = analyze(args.run_dir)
    print(json.dumps({"output": str((args.run_dir / 'timeline-summary.json').resolve()),
                      "rows_read": result["rows_read"],
                      "dropped": result["trace_health"]["max_dropped"],
                      "truncated": result["trace_health"]["truncated"]}))
