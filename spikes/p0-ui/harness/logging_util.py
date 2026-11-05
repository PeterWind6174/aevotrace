"""Structured logging for the P0-UI-01 harness.

Evidence is deliberately kept as both:
  - events.jsonl: machine-readable, one event per line
  - run.log: human-readable ordered trace
  - summary.json: final business state, counters, and backend proof
"""
from __future__ import annotations

import json
import os
import threading
import time


class RunLogger:
    def __init__(self, out_dir: str, backend: str):
        self.out_dir = out_dir
        os.makedirs(out_dir, exist_ok=True)
        self.backend = backend
        self.started = time.time()
        self.started_monotonic = time.monotonic()
        self.lock = threading.Lock()
        self.events_path = os.path.join(out_dir, "events.jsonl")
        self.log_path = os.path.join(out_dir, "run.log")
        self._events = open(self.events_path, "w", encoding="utf-8")
        self._log = open(self.log_path, "w", encoding="utf-8")
        self.error = None
        self.kind_counts: dict[str, int] = {}

    def event(self, event_type: str, **fields):
        now = time.monotonic() - self.started_monotonic
        record = {
            "t": round(now, 4),
            "kind": event_type,
            "backend": self.backend,
            "thread": threading.current_thread().name,
        }
        record.update(fields)
        with self.lock:
            self.kind_counts[event_type] = self.kind_counts.get(event_type, 0) + 1
            self._events.write(json.dumps(record, ensure_ascii=False, default=str) + "\n")
            self._events.flush()
            detail = " ".join(f"{k}={v}" for k, v in fields.items())
            self._log.write(f"{now:9.3f} {event_type:28s} [{threading.current_thread().name}] {detail}\n")
            self._log.flush()

    def close(self):
        with self.lock:
            self._events.close()
            self._log.close()

    def write_summary(self, summary: dict):
        summary = dict(summary)
        summary.setdefault("backend", self.backend)
        summary.setdefault("started_wall", time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(self.started)))
        summary.setdefault("duration_s", round(time.monotonic() - self.started_monotonic, 3))
        summary.setdefault("error", self.error)
        path = os.path.join(self.out_dir, "summary.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2, ensure_ascii=False, default=str)
            f.write("\n")
        return path
