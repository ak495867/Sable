"""SABLE v1.0 adaptive database engine.

The engine keeps SQLite as its conservative transactional kernel while making
planning, telemetry, workload phases, and physical-design decisions explicit.
"""
from __future__ import annotations
import hashlib, json, sqlite3, statistics, time
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator
from .research import CompressionCodec, JoinExecutor, SegmentStore, ShadowExecutor, TabularQLearner, UCB1Bandit, VectorizedExecutor

@dataclass
class Telemetry:
    query_id: int
    fingerprint: str
    plan: str
    rows_scanned: int
    rows_returned: int
    latency_us: int
    index_used: bool
    phase: str
    adaptation_epoch: int
    operation: str = "query"
    cache_hit: bool = False
    bytes_read: int = 0
    bytes_written: int = 0
    cpu_time_us: int = 0
    candidate_plans: tuple[str, ...] = ()

class WorkloadModel:
    """Online workload statistics with a bounded recent window for phase drift."""
    def __init__(self, window: int = 100):
        self.window = window; self.total = 0; self.point = 0; self.scan = 0
        self.aggregate = 0; self.join = 0; self.write = 0
        self.latencies: list[float] = []; self.recent: list[str] = []
        self.fingerprints: dict[str, int] = {}; self.plan_counts: dict[str, int] = {}
        self.plan_latency: dict[str, list[int]] = {}
    def observe(self, fp: str, plan: str, latency_us: int, rows_scanned: int,
                rows_returned: int, operation: str = "query"):
        self.total += 1; self.latencies.append(latency_us); self.latencies = self.latencies[-10000:]
        self.recent.append(plan); self.recent = self.recent[-self.window:]
        self.fingerprints[fp] = self.fingerprints.get(fp, 0) + 1
        self.plan_counts[plan] = self.plan_counts.get(plan, 0) + 1
        self.plan_latency.setdefault(plan, []).append(latency_us)
        self.plan_latency[plan] = self.plan_latency[plan][-1000:]
        if plan in ("index_lookup", "point_lookup"): self.point += 1
        if plan == "scan": self.scan += 1
        if plan == "aggregate": self.aggregate += 1
        if plan == "join": self.join += 1
        if operation in ("insert", "update", "delete"): self.write += 1
    def phase(self) -> str:
        if not self.recent: return "startup"
        counts = {p: self.recent.count(p) for p in set(self.recent)}
        dominant = max(counts, key=counts.get)
        return {"point_lookup":"point_lookups", "index_lookup":"point_lookups",
                "scan":"scans", "aggregate":"analytics", "join":"joins"}.get(dominant, "writes" if self.write > self.total/2 else "mixed")
    def snapshot(self) -> dict[str, Any]:
        n = max(self.total, 1); recent_n = max(len(self.recent), 1)
        recent_point = sum(p in ("point_lookup", "index_lookup") for p in self.recent) / recent_n
        recent_scan = self.recent.count("scan") / recent_n
        return {"queries": self.total, "point_lookup_ratio": self.point/n, "scan_ratio": self.scan/n,
                "aggregate_ratio": self.aggregate/n, "join_ratio": self.join/n, "write_ratio": self.write/n,
                "recent_point_lookup_ratio": recent_point, "recent_scan_ratio": recent_scan,
                "phase": self.phase(), "p50_latency_us": statistics.median(self.latencies) if self.latencies else 0,
                "p95_latency_us": statistics.quantiles(self.latencies, n=20)[18] if len(self.latencies) >= 20 else (max(self.latencies) if self.latencies else 0),
                "plan_counts": self.plan_counts, "plan_p50_us": {k: statistics.median(v) for k,v in self.plan_latency.items()},
                "top_queries": sorted(self.fingerprints.items(), key=lambda x:x[1], reverse=True)[:10]}

class AdaptiveEngine:
    """Deterministic v1 policy engine with empirical observations and safe proposals."""
    def __init__(self, conn: sqlite3.Connection, epoch_size: int = 100):
        self.conn = conn; self.model = WorkloadModel(); self.epoch_size = max(1, epoch_size)
        self.epoch = 0; self.plan_cache: dict[str, str] = {}; self.telemetry: list[Telemetry] = []
        self.proposals: list[dict[str, Any]] = []; self.indexes: set[str] = set()
        self.layout = "row"; self.compression = "none"; self.prefetch = "adaptive"; self.buffer_policy = "workload-aware"
        self._query_id = 0; self._last_phase = "startup"
        self.vectorized = VectorizedExecutor(); self.joins = JoinExecutor
        self.join_bandit = UCB1Bandit(("nested_loop", "hash", "merge"))
        self.policy_learner = TabularQLearner(("nested_loop", "hash", "merge"))
        self.shadow_enabled = True
        self.policy_mode = "heuristic"
    @staticmethod
    def fingerprint(sql: str, params: Iterable[Any] = ()) -> str:
        normalized = " ".join(sql.lower().split())
        # Parameter values are excluded so equivalent prepared statements share a plan.
        return hashlib.sha256(normalized.encode()).hexdigest()[:16]
    def choose_plan(self, sql: str, params: Iterable[Any] = ()) -> tuple[str, tuple[str, ...], bool]:
        fp = self.fingerprint(sql, params); low = sql.lower(); cached = fp in self.plan_cache
        if cached: return self.plan_cache[fp], (self.plan_cache[fp],), True
        candidates = ["scan"]
        if " join " in low: candidates = ["join", "scan"]
        elif any(x in low for x in ("group by", "avg(", "count(", "sum(", "min(", "max(")): candidates = ["aggregate", "scan"]
        elif " where " in low and "=" in low:
            candidates = (["index_lookup", "point_lookup", "scan"]
                          if "idx_sable_key" in self.indexes else ["point_lookup", "scan"])
        plan = candidates[0]
        # Empirical model: prefer a previously observed plan for this broad operator class.
        known = self.model.plan_latency
        measured = [(statistics.median(known[p]), p) for p in candidates if p in known and known[p]
                    and (p != "index_lookup" or "idx_sable_key" in self.indexes)]
        if measured and len(self.model.latencies) >= 20: plan = min(measured)[1]
        self.plan_cache[fp] = plan; return plan, tuple(candidates), False
    def record(self, telemetry: Telemetry):
        self.telemetry.append(telemetry)
        self.model.observe(telemetry.fingerprint, telemetry.plan, telemetry.latency_us, telemetry.rows_scanned, telemetry.rows_returned, telemetry.operation)
        phase = self.model.phase()
        if phase != self._last_phase and self.model.total > self.epoch_size: self.proposals.append({"type":"phase_change", "from":self._last_phase, "to":phase, "status":"observed", "epoch":self.epoch})
        self._last_phase = phase
        if self.model.total % self.epoch_size == 0: self.adapt()
    def adapt(self):
        self.epoch += 1; s = self.model.snapshot(); recent_point = s["recent_point_lookup_ratio"]; recent_scan = s["recent_scan_ratio"]
        if recent_point > .60 and "idx_sable_key" not in self.indexes:
            self._propose_once("create_index", "recent point lookup ratio exceeded 60%", {"index":"idx_sable_key", "benefit":"key equality access"})
        if recent_scan > .70 and self.layout != "column":
            self._propose_once("layout", "recent scan ratio exceeded 70%", {"target":"column", "adaptation_cost":"segment rewrite"})
        if s["write_ratio"] > .50 and self.compression != "none":
            self._propose_once("compression", "write ratio exceeded 50%", {"target":"none", "reason":"reduce write amplification"})
        if s["aggregate_ratio"] + s["scan_ratio"] > .60:
            self._propose_once("prefetch", "scan/analytics workload detected", {"target":"sequential"})
    def _propose_once(self, typ: str, reason: str, details: dict[str, Any]):
        if any(p["type"] == typ and p["status"] == "proposed" for p in self.proposals): return
        self.proposals.append({"type":typ, "reason":reason, "details":details, "status":"proposed", "epoch":self.epoch})
    def approve_proposal(self, n: int = -1):
        pending = [p for p in self.proposals if p["status"] == "proposed"]
        if not pending: return None
        p = pending[n]
        if p["type"] == "create_index":
            self.conn.execute("CREATE INDEX IF NOT EXISTS idx_sable_key ON sable_records(key)"); self.conn.commit(); self.indexes.add("idx_sable_key")
        elif p["type"] == "layout": self.layout = p["details"]["target"]
        elif p["type"] == "compression": self.compression = p["details"]["target"]
        elif p["type"] == "prefetch": self.prefetch = p["details"]["target"]
        p["status"] = "committed"; p["committed_at"] = time.time(); self.plan_cache.clear(); return p
    def reject_proposal(self, n: int = -1):
        pending = [p for p in self.proposals if p["status"] == "proposed"]
        if not pending: return None
        p = pending[n]; p["status"] = "rejected"; return p
    def enable_policy(self, mode: str):
        if mode not in ("heuristic", "bandit", "q_learning"): raise ValueError("policy must be heuristic, bandit, or q_learning")
        self.policy_mode = mode; self.plan_cache.clear(); return mode
    def choose_join_algorithm(self, left_rows, right_rows, key, sorted_inputs=False):
        candidates=("nested_loop", "hash", "merge")
        if self.policy_mode == "bandit": return self.join_bandit.select()
        if self.policy_mode == "q_learning": return self.policy_learner.choose(("join", len(left_rows)//1000, len(right_rows)//1000), explore=False)
        return JoinExecutor.choose(left_rows, right_rows, sorted_inputs)
    def observe_join(self, algorithm: str, elapsed_us: int):
        if algorithm not in ("nested_loop", "hash", "merge"): raise ValueError("unknown join algorithm")
        self.join_bandit.update(algorithm, -float(elapsed_us)); self.policy_learner.update(("join",), algorithm, -float(elapsed_us), ("join",))
    def profile_compression(self, values): return CompressionCodec.profile(values)
    def shadow_validate(self, baseline, candidate, repetitions=3): return ShadowExecutor.compare(baseline, candidate, repetitions=repetitions)
    def status(self):
        return {"version":"1.0.0", "epoch":self.epoch, "policy":self.policy_mode, "physical":{"layout":self.layout, "compression":self.compression, "prefetch":self.prefetch, "buffer_policy":self.buffer_policy}, "research":{"storage_modes":["row","column","hybrid"], "vector_backend":self.vectorized.backend, "join_algorithms":["nested_loop","hash","merge"], "compression_codecs":list(CompressionCodec.codecs), "shadow_enabled":self.shadow_enabled, "bandit":self.join_bandit.status(), "q_states":len(self.policy_learner.q)}, "workload":self.model.snapshot(), "proposals":self.proposals[-20:], "active_indexes":sorted(self.indexes), "telemetry_events":len(self.telemetry)}

class SableDB:
    def __init__(self, path: str = ":memory:", epoch_size: int = 100):
        self.path = path; self.conn = sqlite3.connect(path); self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL"); self.conn.execute("PRAGMA synchronous=NORMAL")
        self.conn.execute("CREATE TABLE IF NOT EXISTS sable_records (id INTEGER PRIMARY KEY, key TEXT UNIQUE, value TEXT, created_at REAL)")
        self.conn.commit(); self.adaptive = AdaptiveEngine(self.conn, epoch_size)
        if self._has_index("idx_sable_key"): self.adaptive.indexes.add("idx_sable_key")
    def _has_index(self, name: str) -> bool:
        return bool(self.conn.execute("SELECT 1 FROM sqlite_master WHERE type='index' AND name=?", (name,)).fetchone())
    @contextmanager
    def transaction(self) -> Iterator["SableDB"]:
        self.conn.execute("BEGIN");
        try: yield self; self.conn.commit()
        except Exception: self.conn.rollback(); raise
    def execute(self, sql: str, params: Iterable[Any] = ()) -> list[dict[str, Any]]:
        params = tuple(params); start = time.perf_counter_ns(); was_in_transaction = self.conn.in_transaction
        plan, candidates, cache_hit = self.adaptive.choose_plan(sql, params)
        cur = self.conn.execute(sql, params); rows = [dict(r) for r in cur.fetchall()] if cur.description else []
        operation = sql.lstrip().split(None, 1)[0].lower() if sql.strip() else "query"
        if operation in ("insert", "update", "delete", "create", "drop", "replace") and not was_in_transaction: self.conn.commit()
        elapsed = max(1, (time.perf_counter_ns() - start) // 1000); self.adaptive._query_id += 1
        scanned = max(cur.rowcount if cur.rowcount >= 0 else len(rows), len(rows), 0)
        self.adaptive.record(Telemetry(self.adaptive._query_id, self.adaptive.fingerprint(sql, params), plan, scanned, len(rows), elapsed, plan == "index_lookup", self.adaptive.model.phase(), self.adaptive.epoch, operation, cache_hit, len(str(rows).encode()), 0, elapsed, candidates))
        return rows
    def put(self, key: str, value: Any):
        self.execute("INSERT INTO sable_records(key,value,created_at) VALUES(?,?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, json.dumps(value), time.time()))
    def bulk_put(self, items: Iterable[tuple[str, Any]]):
        start = time.perf_counter_ns(); count = 0
        with self.conn:
            for key, value in items:
                self.conn.execute("INSERT INTO sable_records(key,value,created_at) VALUES(?,?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, json.dumps(value), time.time())); count += 1
        elapsed = max(1, (time.perf_counter_ns()-start)//1000); self.adaptive._query_id += 1
        self.adaptive.record(Telemetry(self.adaptive._query_id, "bulk_put", "write", count, 0, elapsed, False, "writes", self.adaptive.epoch, "insert", False, 0, count, elapsed, ("write",)))
        return count
    def get(self, key: str):
        rows = self.execute("SELECT key,value,created_at FROM sable_records WHERE key = ?", (key,)); return json.loads(rows[0]["value"]) if rows else None
    def close(self): self.conn.close()
    def export_telemetry(self, path: str):
        Path(path).write_text("\n".join(json.dumps(asdict(t)) for t in self.adaptive.telemetry) + ("\n" if self.adaptive.telemetry else ""))
    def export_summary(self, path: str): Path(path).write_text(json.dumps(self.adaptive.status(), indent=2, default=str) + "\n")
    def create_segment_store(self, path: str, mode="row", segment_rows=4096): return SegmentStore(path, mode, segment_rows)
    def vectorized(self): return self.adaptive.vectorized
