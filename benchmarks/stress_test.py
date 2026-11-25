import argparse
import concurrent.futures
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1]))
from sable import SableDB, LinUCBBandit, JoinExecutor, CompressionCodec, SegmentStore


def run_concurrency_stress(db_path, workers=4, items_per_worker=500):
    start = time.perf_counter()
    errors = 0
    total_ops = workers * items_per_worker

    def worker_task(worker_id):
        nonlocal errors
        db = SableDB(db_path, epoch_size=50)
        try:
            for i in range(items_per_worker):
                k = f"w{worker_id}:k{i}"
                db.put(k, {"worker": worker_id, "seq": i, "payload": "x" * 64})
                val = db.get(k)
                if not val or val.get("seq") != i:
                    errors += 1
        except Exception:
            errors += 1
        finally:
            db.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(worker_task, w) for w in range(workers)]
        concurrent.futures.wait(futures)

    elapsed = time.perf_counter() - start
    ops_sec = total_ops * 2 / max(elapsed, 0.001)
    return {
        "workers": workers,
        "total_ops": total_ops * 2,
        "elapsed_sec": round(elapsed, 3),
        "ops_per_sec": round(ops_sec, 1),
        "errors": errors,
    }


def run_phase_oscillation_stress(db_path, scale=1):
    db = SableDB(db_path, epoch_size=100)
    results = {}
    n_writes = 5000 * scale
    n_lookups = 8000 * scale
    n_scans = 200 * scale
    n_aggs = 200 * scale

    t0 = time.perf_counter()
    batch = [(f"rec:{i}", {"val": i, "bucket": i % 10, "tag": "test"}) for i in range(n_writes)]
    db.bulk_put(batch)
    results["bulk_write_sec"] = round(time.perf_counter() - t0, 3)

    t1 = time.perf_counter()
    for i in range(n_lookups):
        db.get(f"rec:{(i * 17) % n_writes}")
    results["point_lookups_sec"] = round(time.perf_counter() - t1, 3)

    pending = [p for p in db.adaptive.proposals if p["status"] == "proposed"]
    approved = []
    for _ in range(len(pending)):
        p = db.adaptive.approve_proposal()
        if p:
            approved.append(p["type"])
    results["approved_proposals"] = approved

    t2 = time.perf_counter()
    for _ in range(n_scans):
        db.execute("SELECT key, value FROM sable_records WHERE key LIKE ?", ("rec:%",))
    results["scans_sec"] = round(time.perf_counter() - t2, 3)

    t3 = time.perf_counter()
    for _ in range(n_aggs):
        db.execute("SELECT COUNT(*) AS total FROM sable_records")
    results["aggs_sec"] = round(time.perf_counter() - t3, 3)

    t4 = time.perf_counter()
    heavy_writes = [(f"heavy:{i}", {"val": i, "tag": "heavy"}) for i in range(n_writes)]
    db.bulk_put(heavy_writes)
    for i in range(200):
        db.put(f"burst:{i}", i)
    db.adaptive.adapt()
    results["heavy_writes_sec"] = round(time.perf_counter() - t4, 3)

    status = db.adaptive_status()
    results["active_indexes"] = status["active_indexes"]
    results["epochs"] = status["epoch"]
    results["total_telemetry"] = status["telemetry_events"]
    results["p50_latency_us"] = status["workload"]["p50_latency_us"]
    results["p95_latency_us"] = status["workload"]["p95_latency_us"]
    db.close()
    return results


def run_tier_migration_stress(db_path, record_count=3000):
    db = SableDB(db_path, epoch_size=50)
    items = [(f"tier:{i}", {"seq": i, "data": "payload_" + str(i % 50)}) for i in range(record_count)]
    db.bulk_put(items)

    archived = db.archive_cold_records(limit=record_count // 2)

    hits_active = 0
    hits_archived = 0
    for i in range(0, record_count, 10):
        val = db.get(f"tier:{i}")
        if val is not None:
            if i < record_count // 2:
                hits_archived += 1
            else:
                hits_active += 1

    checksum_errors = 0
    if db.adaptive.storage_classifier:
        for seg in db.adaptive.storage_classifier.cold_store.segments:
            try:
                db.adaptive.storage_classifier.cold_store._read_segment(seg)
            except Exception:
                checksum_errors += 1

    db.close()
    return {
        "total_records": record_count,
        "archived_records": archived,
        "hits_active": hits_active,
        "hits_archived": hits_archived,
        "checksum_errors": checksum_errors,
    }


def run_policy_learning_stress(decisions=2000):
    bandit = LinUCBBandit(["nested_loop", "hash", "merge"], dim=3)
    left_small = [{"id": i, "x": i} for i in range(20)]
    right_small = [{"id": i, "y": i} for i in range(20)]
    left_large = [{"id": i, "x": i} for i in range(500)]
    right_large = [{"id": i, "y": i} for i in range(500)]

    for step in range(decisions):
        is_small = step % 2 == 0
        left = left_small if is_small else left_large
        right = right_small if is_small else right_large
        ctx = [1.0, len(left) / 100.0, len(right) / 100.0]
        arm = bandit.select(ctx)
        t = time.perf_counter_ns()
        JoinExecutor.execute(left, right, "id", arm)
        elapsed = (time.perf_counter_ns() - t) // 1000
        bandit.update(arm, ctx, -float(elapsed) / 100.0)

    ctx_small = [1.0, 0.2, 0.2]
    ctx_large = [1.0, 5.0, 5.0]
    return {
        "decisions": decisions,
        "small_workload_preferred": bandit.select(ctx_small),
        "large_workload_preferred": bandit.select(ctx_large),
        "bandit_status": bandit.status(),
    }


def main():
    parser = argparse.ArgumentParser(description="SABLE Maximum Engine Stress Test")
    parser.add_argument("--scale", type=int, default=1)
    parser.add_argument("--db", default="stress_run.db")
    args = parser.parse_args()

    db_path = Path(args.db)
    if db_path.exists():
        db_path.unlink()

    print("==================================================")
    print("      SABLE MAXIMUM ENGINE STRESS TEST SUITE      ")
    print("==================================================")

    print("\n[Phase 1] High-Concurrency Multithreaded Ingestion & Read Bursts...")
    c_res = run_concurrency_stress(str(db_path), workers=4, items_per_worker=500 * args.scale)
    print(f"  Ops Completed : {c_res['total_ops']}")
    print(f"  Throughput    : {c_res['ops_per_sec']} ops/sec")
    print(f"  Errors        : {c_res['errors']}")
    assert c_res["errors"] == 0

    print("\n[Phase 2] Massive Volume & Rapid Phase Oscillations...")
    p_res = run_phase_oscillation_stress(str(db_path), scale=args.scale)
    print(f"  Bulk Writes   : {p_res['bulk_write_sec']}s")
    print(f"  Lookups       : {p_res['point_lookups_sec']}s")
    print(f"  Scans         : {p_res['scans_sec']}s")
    print(f"  Aggregations  : {p_res['aggs_sec']}s")
    print(f"  Proposals     : {p_res['approved_proposals']}")
    print(f"  p50 Latency   : {p_res['p50_latency_us']} us")
    print(f"  p95 Latency   : {p_res['p95_latency_us']} us")

    print("\n[Phase 3] Storage Tier Archiving & Segment Verification...")
    t_res = run_tier_migration_stress(str(db_path), record_count=3000 * args.scale)
    print(f"  Total Records : {t_res['total_records']}")
    print(f"  Archived Cold : {t_res['archived_records']}")
    print(f"  Archived Hits : {t_res['hits_archived']}")
    print(f"  Active Hits   : {t_res['hits_active']}")
    print(f"  Checksum Errs : {t_res['checksum_errors']}")
    assert t_res["checksum_errors"] == 0

    print("\n[Phase 4] Contextual LinUCB Join Policy Learning...")
    l_res = run_policy_learning_stress(decisions=1000 * args.scale)
    print(f"  Decisions     : {l_res['decisions']}")
    print(f"  Small Workload: {l_res['small_workload_preferred']}")
    print(f"  Large Workload: {l_res['large_workload_preferred']}")

    print("\n[Phase 5] State Persistence & Recovery Validation...")
    reopened = SableDB(str(db_path))
    recovered_events = reopened.adaptive.status()["telemetry_events"]
    print(f"  Recovered Telemetry Events: {recovered_events}")
    assert recovered_events > 0
    reopened.close()

    if db_path.exists():
        db_path.unlink()

    print("\n==================================================")
    print("      ALL STRESS PHASES PASSED WITH ZERO ERRORS   ")
    print("==================================================")


if __name__ == "__main__":
    main()
