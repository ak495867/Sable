import json
import sys
from pathlib import Path
from sable import SableDB

def main():
    db_path = sys.argv[1] if len(sys.argv) > 1 else "demo.db"
    db_file = Path(db_path)
    if db_file.exists():
        db_file.unlink()
    db = SableDB(str(db_file), epoch_size=20)
    print("Engine initialized on:", db_path)

    items = [
        ("trade:1", {"symbol": "AAPL", "price": 227.10, "volume": 1500}),
        ("trade:2", {"symbol": "MSFT", "price": 428.50, "volume": 800}),
        ("trade:3", {"symbol": "GOOGL", "price": 178.20, "volume": 2200}),
        ("trade:4", {"symbol": "NVDA", "price": 128.90, "volume": 5000}),
        ("trade:5", {"symbol": "AMZN", "price": 186.40, "volume": 1200}),
    ]
    for i in range(6, 51):
        items.append((f"trade:{i}", {"symbol": ["AAPL", "MSFT", "GOOGL", "NVDA", "AMZN"][i % 5], "price": 100.0 + i, "volume": i * 10}))

    count = db.bulk_put(items)
    print(f"Ingested {count} records.")

    print("\nExecuting point lookup workload...")
    for _ in range(5):
        for i in range(1, 10):
            res = db.get(f"trade:{i}")
    print("Point lookups completed.")

    status = db.adaptive_status()
    print("\nWorkload Snapshot:")
    print(json.dumps(status["workload"], indent=2, default=str))

    pending = [p for p in db.adaptive.proposals if p["status"] == "proposed"]
    print(f"\nPending Proposals: {len(pending)}")
    for p in pending:
        print(f" - Type: {p['type']}, Reason: {p['reason']}")

    if pending:
        approved = db.adaptive.approve_proposal()
        print(f"\nApproved Proposal: {approved['type']} -> Status: {approved['status']}")
        print(f"Active Indexes: {db.adaptive.status()['active_indexes']}")

    print("\nExecuting scan workload...")
    for _ in range(30):
        db.execute("SELECT key, value FROM sable_records WHERE key LIKE ?", ("trade:%",))

    print("\nExecuting analytics workload...")
    for _ in range(30):
        db.execute("SELECT COUNT(*) AS total FROM sable_records")

    print("\nAutonomous Subsystems Status:")
    print("Access Tracker:", json.dumps(db.access_tracker_status(), indent=2))
    print("Cost Model:", json.dumps(db.cost_model_status(), indent=2))
    print("Execution Engine:", json.dumps(db.execution_status(), indent=2))

    telemetry_path = "demo_telemetry.jsonl"
    db.export_telemetry(telemetry_path)
    print(f"\nExported {len(db.adaptive.telemetry)} telemetry records to {telemetry_path}")

    db.close()
    print("Engine closed cleanly and state persisted.")

if __name__ == "__main__":
    main()
