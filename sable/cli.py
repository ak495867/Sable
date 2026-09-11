from __future__ import annotations
import argparse, json
from .core import SableDB

def main():
    p=argparse.ArgumentParser(prog="sable", description="SABLE self-optimizing database v1.0")
    p.add_argument("path", nargs="?", default="sable.db"); sub=p.add_subparsers(dest="cmd")
    sub.add_parser("status"); q=sub.add_parser("query"); q.add_argument("sql"); q.add_argument("params", nargs="*")
    put=sub.add_parser("put"); put.add_argument("key"); put.add_argument("value")
    sub.add_parser("approve"); exp=sub.add_parser("export"); exp.add_argument("file")
    a=p.parse_args(); db=SableDB(a.path)
    try:
        if a.cmd=="status": print(json.dumps(db.adaptive.status(), indent=2, default=str))
        elif a.cmd=="query": print(json.dumps(db.execute(a.sql, a.params), indent=2, default=str))
        elif a.cmd=="put": db.put(a.key,a.value); print("ok")
        elif a.cmd=="approve": print(json.dumps(db.adaptive.approve_proposal(), indent=2))
        elif a.cmd=="export": db.export_telemetry(a.file); print(a.file)
        else: p.print_help()
    finally: db.close()
if __name__ == "__main__": main()
