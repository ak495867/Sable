import argparse, json, random, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1]))
from sable import SableDB


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rows", type=int, default=20000)
    p.add_argument("--epoch-size", type=int, default=100)
    p.add_argument("--telemetry", default="benchmark-telemetry.jsonl")
    p.add_argument("--summary", default="benchmark-summary.json")
    a = p.parse_args()
    db = SableDB(":memory:", epoch_size=a.epoch_size)
    db.bulk_put(
        (
            f"row:{i}",
            {"symbol": ["AAPL", "MSFT", "NVDA"][i % 3], "value": i, "bucket": i % 100},
        )
        for i in range(a.rows)
    )
    # Phase 1: point lookups. This should eventually propose the key index.
    for i in range(min(a.rows, 5000)):
        db.get(f"row:{(i * 17) % a.rows}")
    # Phase 2: broad scans.
    for _ in range(300):
        db.execute("SELECT key, value FROM sable_records WHERE key LIKE ?", ("row:%",))
    # Phase 3: analytics.
    for _ in range(300):
        db.execute("SELECT COUNT(*) AS n FROM sable_records")
    # Phase 4: writes.
    db.bulk_put((f"update:{i}", {"value": i}) for i in range(min(a.rows, 2500)))
    db.export_telemetry(a.telemetry)
    db.export_summary(a.summary)
    print(
        json.dumps(
            {
                "benchmark": "phase_change",
                "rows": a.rows,
                "telemetry": a.telemetry,
                "summary": a.summary,
                "status": db.adaptive.status(),
            },
            indent=2,
            default=str,
        )
    )


if __name__ == "__main__":
    main()
