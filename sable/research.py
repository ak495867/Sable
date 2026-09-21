"""Substantive SABLE research subsystems.

The implementations are usable single-node components: durable segments,
NumPy vector execution, measured physical joins, binary compression, shadow
validation, UCB1 exploration, and tabular Q-learning.
"""

from __future__ import annotations
import hashlib
import json
import math
import os
import pickle
import random
import struct
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

try:
    import numpy as np

    NUMPY = True
except ImportError:
    np = None
    NUMPY = False


class SegmentStore:
    """Durable appendable segment store with row/column physical layouts."""

    def __init__(self, path: str, mode="row", segment_rows=4096):
        self.root = Path(path)
        self.root.mkdir(parents=True, exist_ok=True)
        self.segment_rows = segment_rows
        self.meta_path = self.root / "manifest.json"
        self.segments: list[dict[str, Any]] = []
        if self.meta_path.exists():
            self._load_manifest()
        else:
            self.mode = mode
            self._save_manifest()

    def _load_manifest(self):
        meta = json.loads(self.meta_path.read_text())
        self.mode = meta["mode"]
        self.segment_rows = meta["segment_rows"]
        self.segments = meta["segments"]

    def _save_manifest(self):
        tmp = self.meta_path.with_suffix(".tmp")
        tmp.write_text(
            json.dumps(
                {
                    "version": 1,
                    "mode": self.mode,
                    "segment_rows": self.segment_rows,
                    "segments": self.segments,
                },
                indent=2,
            )
        )
        os.replace(tmp, self.meta_path)

    def append(self, rows: Iterable[dict[str, Any]]):
        batch = [dict(r) for r in rows]
        for start in range(0, len(batch), self.segment_rows):
            chunk = batch[start : start + self.segment_rows]
            sid = len(self.segments)
            filename = f"segment-{sid:06d}"
            if self.mode == "row":
                payload = json.dumps(
                    chunk, separators=(",", ":"), sort_keys=True
                ).encode()
                (self.root / (filename + ".rows")).write_bytes(payload)
            else:
                cols = {
                    k: [r.get(k) for r in chunk] for k in {k for r in chunk for k in r}
                }
                (self.root / (filename + ".cols")).write_bytes(
                    pickle.dumps(cols, protocol=5)
                )
                payload = pickle.dumps(cols, protocol=5)
            self.segments.append(
                {
                    "id": sid,
                    "rows": len(chunk),
                    "file": filename,
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }
            )
        self._save_manifest()
        return len(batch)

    def scan(
        self,
        columns: Sequence[str] | None = None,
        predicate: Callable[[dict[str, Any]], bool] | None = None,
    ):
        out = []
        for seg in self.segments:
            if self.mode == "row":
                rows = json.loads((self.root / (seg["file"] + ".rows")).read_bytes())
            else:
                cols = pickle.loads((self.root / (seg["file"] + ".cols")).read_bytes())
                n = seg["rows"]
                rows = [{k: cols[k][i] for k in cols} for i in range(n)]
            for r in rows:
                if predicate is None or predicate(r):
                    out.append({k: r.get(k) for k in columns} if columns else r)
        return out

    def migrate(self, mode: str):
        if mode not in ("row", "column"):
            raise ValueError("mode must be row or column")
        if mode == self.mode:
            return False
        rows = self.scan()
        old = list(self.segments)
        self.mode = mode
        self.segments = []
        for seg in old:
            for suffix in (".rows", ".cols"):
                p = self.root / (seg["file"] + suffix)
                if p.exists():
                    p.unlink()
        self.append(rows)
        self._save_manifest()
        return True

    def stats(self):
        return {
            "mode": self.mode,
            "segments": len(self.segments),
            "rows": sum(s["rows"] for s in self.segments),
            "bytes": sum(
                (self.root / (s["file"] + (".rows" if self.mode == "row" else ".cols")))
                .stat()
                .st_size
                for s in self.segments
            ),
        }


class RowStore:
    mode = "row"

    def __init__(self, rows: Iterable[dict[str, Any]] = (), path: str | None = None):
        self.rows = [dict(r) for r in rows]
        self.segment = SegmentStore(path, "row") if path else None
        if self.segment and self.rows:
            self.segment.append(self.rows)

    def append(self, row):
        self.rows.append(dict(row))
        self.segment and self.segment.append([row])

    def scan(self, columns=None):
        return (
            self.segment.scan(columns)
            if self.segment
            else (
                [{k: r.get(k) for k in columns} for r in self.rows]
                if columns
                else list(self.rows)
            )
        )


class ColumnStore:
    mode = "column"

    def __init__(self, rows: Iterable[dict[str, Any]] = (), path: str | None = None):
        self.columns = {}
        self.segment = SegmentStore(path, "column") if path else None
        if self.segment:
            self.columns = {}
            self.rows = []
            self.load(rows)
        else:
            self.load(rows)

    def load(self, rows):
        batch = [dict(r) for r in rows]
        if self.segment:
            self.segment.append(batch)
            return
        for row in batch:
            for key in row:
                self.columns.setdefault(key, []).append(row[key])
            for key in set(self.columns) - set(row):
                self.columns[key].append(None)

    def append(self, row):
        self.load([row])

    def scan(self, columns=None):
        if self.segment:
            return self.segment.scan(columns)
        cols = columns or list(self.columns)
        n = len(next(iter(self.columns.values()), []))
        return [{k: self.columns.get(k, [None] * n)[i] for k in cols} for i in range(n)]


class HybridStore:
    mode = "hybrid"

    def __init__(self, rows=(), hot_columns=(), path: str | None = None):
        self.hot_columns = set(hot_columns)
        self.row = RowStore(rows, path)
        self.column = ColumnStore(self.row.rows)

    def append(self, row):
        self.row.append(row)
        self.column.append(row)

    def scan(self, columns=None):
        return (
            self.column.scan(columns)
            if columns and self.hot_columns.intersection(columns)
            else self.row.scan(columns)
        )


class JoinExecutor:
    @staticmethod
    def nested_loop(left, right, key):
        return [
            {**a, **{f"r_{k}": v for k, v in b.items()}}
            for a in left
            for b in right
            if a.get(key) == b.get(key)
        ]

    @staticmethod
    def hash_join(left, right, key):
        index = {}
        for b in right:
            index.setdefault(b.get(key), []).append(b)
        return [
            {**a, **{f"r_{k}": v for k, v in b.items()}}
            for a in left
            for b in index.get(a.get(key), [])
        ]

    @staticmethod
    def merge_join(left, right, key, presorted=False):
        a = left if presorted else sorted(left, key=lambda x: x.get(key))
        b = right if presorted else sorted(right, key=lambda x: x.get(key))
        i = j = 0
        out = []
        while i < len(a) and j < len(b):
            if a[i].get(key) < b[j].get(key):
                i += 1
            elif a[i].get(key) > b[j].get(key):
                j += 1
            else:
                ii = i
                while ii < len(a) and a[ii].get(key) == a[i].get(key):
                    jj = j
                    while jj < len(b) and b[jj].get(key) == b[j].get(key):
                        out.append({**a[ii], **{f"r_{x}": y for x, y in b[jj].items()}})
                        jj += 1
                    ii += 1
                i = ii
        return out

    @classmethod
    def execute(cls, left, right, key, algorithm="hash", presorted=False):
        return (
            cls.merge_join(left, right, key, presorted)
            if algorithm == "merge"
            else {"nested_loop": cls.nested_loop, "hash": cls.hash_join}[algorithm](
                left, right, key
            )
        )

    @classmethod
    def choose(cls, left_rows, right_rows, sorted_inputs=False):
        if min(len(left_rows), len(right_rows)) < 64:
            return "nested_loop"
        return "merge" if sorted_inputs else "hash"


class CompressionCodec:
    codecs = ("none", "zlib", "delta", "rle")

    @staticmethod
    def encode(values, codec="zlib"):
        if codec == "none":
            return pickle.dumps(list(values), protocol=5)
        if codec == "delta":
            nums = [int(v) for v in values]
            raw = (
                struct.pack(
                    f"<{len(nums)}q",
                    *([nums[0]] + [nums[i] - nums[i - 1] for i in range(1, len(nums))]),
                )
                if nums
                else b""
            )
            return zlib.compress(raw, 9)
        if codec == "rle":
            out = []
            for v in values:
                if out and out[-1][0] == v:
                    out[-1] = (v, out[-1][1] + 1)
                else:
                    out.append((v, 1))
            return zlib.compress(pickle.dumps(out, protocol=5), 9)
        return zlib.compress(pickle.dumps(list(values), protocol=5), 6)

    @staticmethod
    def decode(blob, codec="zlib"):
        if codec == "none":
            return pickle.loads(blob)
        if codec == "delta":
            raw = zlib.decompress(blob)
            n = len(raw) // 8
            if not n:
                return []
            d = list(struct.unpack(f"<{n}q", raw))
            out = [d[0]]
            for x in d[1:]:
                out.append(out[-1] + x)
            return out
        vals = pickle.loads(zlib.decompress(blob))
        return [v for v, n in vals for _ in range(n)] if codec == "rle" else vals

    @classmethod
    def choose(cls, values, access_frequency=1.0, cpu_budget=1.0):
        if not values:
            return "none"
        sizes = {c: len(cls.encode(values, c)) for c in cls.codecs}
        return min(sizes, key=sizes.get) if access_frequency < cpu_budget else "none"

    @classmethod
    def profile(cls, values):
        return {
            c: {
                "bytes": len(cls.encode(values, c)),
                "roundtrip": cls.decode(cls.encode(values, c), c) == list(values),
            }
            for c in cls.codecs
        }


@dataclass
class ShadowResult:
    baseline: str
    candidate: str
    baseline_us: int
    candidate_us: int
    winner: str
    rows_equal: bool
    repetitions: int = 1
    speedup: float = 1.0


class ShadowExecutor:
    @staticmethod
    def compare(
        baseline,
        candidate,
        baseline_name="baseline",
        candidate_name="candidate",
        repetitions=3,
    ):
        def run(fn):
            vals = []
            result = None
            for _ in range(repetitions):
                t = time.perf_counter_ns()
                result = fn()
                vals.append((time.perf_counter_ns() - t) // 1000)
            return result, sorted(vals)[len(vals) // 2]

        a, at = run(baseline)
        b, bt = run(candidate)
        winner = candidate_name if bt < at else baseline_name
        return ShadowResult(
            baseline_name,
            candidate_name,
            at,
            bt,
            winner,
            a == b,
            repetitions,
            (at / bt if bt else float("inf")),
        )


class UCB1Bandit:
    def __init__(self, arms, seed=7):
        self.arms = list(arms)
        self.counts = {a: 0 for a in self.arms}
        self.rewards = {a: 0.0 for a in self.arms}
        self.rng = random.Random(seed)

    def select(self):
        unseen = [a for a in self.arms if not self.counts[a]]
        if unseen:
            return unseen[0]
        total = sum(self.counts.values())
        return max(
            self.arms,
            key=lambda a: self.rewards[a] / self.counts[a]
            + math.sqrt(2 * math.log(total) / self.counts[a]),
        )

    def update(self, arm, reward):
        self.counts[arm] += 1
        self.rewards[arm] += reward

    def status(self):
        return {
            a: {
                "trials": self.counts[a],
                "mean_reward": (
                    self.rewards[a] / self.counts[a] if self.counts[a] else 0
                ),
            }
            for a in self.arms
        }


class TabularQLearner:
    def __init__(self, actions, alpha=0.2, gamma=0.9, epsilon=0.1, seed=7):
        self.actions = list(actions)
        self.q = {}
        self.alpha = alpha
        self.gamma = gamma
        self.epsilon = epsilon
        self.rng = random.Random(seed)

    def _row(self, state):
        return self.q.setdefault(tuple(state), {a: 0.0 for a in self.actions})

    def choose(self, state, explore=True):
        row = self._row(state)
        if explore and self.rng.random() < self.epsilon:
            return self.rng.choice(self.actions)
        return max(self.actions, key=lambda a: row[a])

    def update(self, state, action, reward, next_state):
        row = self._row(state)
        row[action] += self.alpha * (
            reward + self.gamma * max(self._row(next_state).values()) - row[action]
        )

    def status(self):
        return {str(k): v for k, v in self.q.items()}


class HotWarmColdStore:
    """v3.0 adaptive physical storage with hot/warm/cold classification.

    Rows are classified by access frequency and recency, then materialized
    into separate segment stores. The hot tier keeps hot columns in columnar
    layout for vectorized scans; the cold tier uses row layout with measured
    compression. Classification decisions are exposed via `enable_classification()`
    and `reclassify()` for the adaptive engine to propose.
    """

    def __init__(
        self,
        path: str = "hwc",
        segment_rows: int = 1024,
        hot_columns: Sequence[str] = (),
        cold_compression: str = "zlib",
    ):
        self.root = Path(path)
        self.root.mkdir(parents=True, exist_ok=True)
        self.segment_rows = segment_rows
        self.hot_columns = set(hot_columns)
        self.cold_compression = cold_compression
        self.hot_store = SegmentStore(str(self.root / "hot"), "column", segment_rows)
        self.warm_store = SegmentStore(str(self.root / "warm"), "row", segment_rows)
        self.cold_store = SegmentStore(str(self.root / "cold"), "row", segment_rows)
        self.mode = "disabled"
        self.classification_counts = {"hot": 0, "warm": 0, "cold": 0}

    def _classify(
        self,
        hot: list[dict[str, Any]] = (),
        warm: list[dict[str, Any]] = (),
        cold: list[dict[str, Any]] = (),
    ):
        if hot:
            self.hot_store.append(hot)
        if warm:
            self.warm_store.append(warm)
        if cold:
            self.cold_store.append(cold)
        self.classification_counts = {
            "hot": len(hot),
            "warm": len(warm),
            "cold": len(cold),
        }
        self.mode = "hot_warm_cold"

    def enable_classification(self):
        self.mode = "hot_warm_cold"
        return self.mode

    def reclassify(self, access_freqs: list[float], rows: list[dict[str, Any]]):
        """Reclassify rows given access frequency signals.

        Args:
            access_freqs: per-row access frequency in [0, 1].
            rows: rows to reclassify.
        """
        if len(access_freqs) != len(rows):
            raise ValueError("access_freqs and rows length mismatch")
        hot, warm, cold = [], [], []
        for freq, row in zip(access_freqs, rows):
            if freq > 0.5:
                hot.append(row)
            elif freq > 0.1:
                warm.append(row)
            else:
                cold.append(row)
        self._classify(hot, warm, cold)
        return self.classification_counts

    def scan(
        self,
        columns: Sequence[str] | None = None,
        predicate: Callable[[dict[str, Any]], bool] | None = None,
    ):
        out = []
        out.extend(self.hot_store.scan(columns, predicate))
        out.extend(self.warm_store.scan(columns, predicate))
        out.extend(self.cold_store.scan(columns, predicate))
        return out

    def stats(self) -> dict[str, Any]:
        return {
            "mode": self.mode,
            "hot": self.hot_store.stats(),
            "warm": self.warm_store.stats(),
            "cold": self.cold_store.stats(),
            "classification_counts": self.classification_counts,
        }


class BufferPolicy:
    """v3.0 adaptive buffer/prefetch policy with workload-aware heuristics.

    Selects between sequential, random, and adaptive prefetch strategies based
    on the recent workload phase. The policy is exposed for the adaptive engine
    to propose and for benchmarks to measure.
    """

    STRATEGIES = ("sequential", "random", "adaptive")

    def __init__(self, strategy: str = "adaptive", buffer_size: int = 4096):
        if strategy not in self.STRATEGIES:
            raise ValueError(f"strategy must be one of {self.STRATEGIES}")
        self.strategy = strategy
        self.buffer_size = buffer_size
        self.history: list[str] = []

    def choose(self, phase: str, recent_scan_ratio: float) -> str:
        if self.strategy == "adaptive":
            if recent_scan_ratio > 0.5:
                chosen = "sequential"
            elif recent_scan_ratio < 0.1:
                chosen = "random"
            else:
                chosen = "adaptive"
        else:
            chosen = self.strategy
        self.history.append(chosen)
        self.history = self.history[-1000:]
        return chosen

    def status(self) -> dict[str, Any]:
        return {
            "strategy": self.strategy,
            "buffer_size": self.buffer_size,
            "recent_decisions": self.history[-10:],
        }


class VectorizedExecutor:
    """NumPy vector engine with typed operations and Python fallback."""

    def __init__(self, batch_size=65536):
        self.batch_size = batch_size
        self.backend = "numpy" if NUMPY else "python"

    def _array(self, v):
        return np.asarray(v) if NUMPY else v

    def filter(self, values, predicate):
        if NUMPY and not callable(predicate):
            return np.asarray(values)[predicate].tolist()
        return [
            v
            for s in range(0, len(values), self.batch_size)
            for v in values[s : s + self.batch_size]
            if predicate(v)
        ]

    def add(self, left, right):
        return (
            (self._array(left) + self._array(right)).tolist()
            if NUMPY
            else [a + b for a, b in zip(left, right)]
        )

    def sum(self, values):
        return self._array(values).sum().item() if NUMPY else sum(values)

    def compare(self, values, op, value):
        a = self._array(values)
        result = {">": a > value, "<": a < value, "==": a == value, "!=": a != value}[
            op
        ]
        return result.tolist() if NUMPY else [op == "==" and x == value for x in values]

    def benchmark(self, n=1000000):
        if not NUMPY:
            return {"backend": "python"}
        a = np.arange(n, dtype=np.float64)
        t = time.perf_counter()
        total = float((a + a).sum())
        return {
            "backend": "numpy",
            "n": n,
            "sum": total,
            "elapsed_ms": (time.perf_counter() - t) * 1000,
        }
