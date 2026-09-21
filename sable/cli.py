from __future__ import annotations
import argparse
import json
from .core import SableDB


def main():
    p = argparse.ArgumentParser(
        prog="sable", description="SABLE v3.0 self-optimizing database"
    )
    p.add_argument("path", nargs="?", default="sable.db")
    sub = p.add_subparsers(dest="cmd")

    sub.add_parser("status")
    sub.add_parser("journal")
    q = sub.add_parser("query")
    q.add_argument("sql")
    q.add_argument("params", nargs="*")
    put = sub.add_parser("put")
    put.add_argument("key")
    put.add_argument("value")
    ap = sub.add_parser("approve")
    ap.add_argument("-n", type=int, default=-1)
    rj = sub.add_parser("reject")
    rj.add_argument("-n", type=int, default=-1)
    sub.add_parser("rollback")
    exp = sub.add_parser("export")
    exp.add_argument("file")
    exp.add_argument("--type", choices=("telemetry", "summary"), default="telemetry")
    pol = sub.add_parser("policy")
    pol.add_argument("mode", choices=("heuristic", "bandit", "q_learning"))
    sub.add_parser("compression")
    sub.add_parser("stats")

    a = p.parse_args()
    db = SableDB(a.path)
    try:
        if a.cmd == "status":
            print(json.dumps(db.adaptive.status(), indent=2, default=str))
        elif a.cmd == "journal":
            print(json.dumps(db.adaptive.adaptation_journal, indent=2, default=str))
        elif a.cmd == "query":
            print(json.dumps(db.execute(a.sql, a.params), indent=2, default=str))
        elif a.cmd == "put":
            db.put(a.key, a.value)
            print("ok")
        elif a.cmd == "approve":
            print(
                json.dumps(db.adaptive.approve_proposal(getattr(a, "n", -1)), indent=2)
            )
        elif a.cmd == "reject":
            print(
                json.dumps(db.adaptive.reject_proposal(getattr(a, "n", -1)), indent=2)
            )
        elif a.cmd == "rollback":
            print(json.dumps(db.rollback_last_adaptation(), indent=2, default=str))
        elif a.cmd == "export":
            if a.type == "telemetry":
                db.export_telemetry(a.file)
            else:
                db.export_summary(a.file)
            print(a.file)
        elif a.cmd == "policy":
            print(json.dumps({"enabled": db.adaptive.enable_policy(a.mode)}, indent=2))
        elif a.cmd == "compression":
            print(
                json.dumps(db.adaptive.recommend_compression([1, 1, 2, 2, 3]), indent=2)
            )
        elif a.cmd == "stats":
            print(
                json.dumps(
                    db.adaptive.status().get("schema", {}), indent=2, default=str
                )
            )
        else:
            p.print_help()
    finally:
        db.close()


if __name__ == "__main__":
    main()
