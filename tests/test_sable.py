import os, tempfile, unittest
from pathlib import Path
from sable import SableDB

class SableTests(unittest.TestCase):
    def test_put_get_and_persistence(self):
        f=tempfile.NamedTemporaryFile(delete=False); f.close()
        try:
            db=SableDB(f.name); db.put("a", {"price": 42}); self.assertEqual(db.get("a"), {"price":42}); db.close()
            db=SableDB(f.name); self.assertEqual(db.get("a"), {"price":42}); db.close()
        finally: os.unlink(f.name)
    def test_transaction_rolls_back(self):
        db=SableDB(":memory:")
        with self.assertRaises(ValueError):
            with db.transaction(): db.put("x", 1); raise ValueError("rollback")
        self.assertIsNone(db.get("x")); db.close()
    def test_bulk_and_adaptation_proposal_commit(self):
        db=SableDB(":memory:", epoch_size=5); self.assertEqual(db.bulk_put((f"k{i}", i) for i in range(20)), 20)
        for i in range(40): db.get(f"k{i % 20}")
        self.assertTrue(db.adaptive.proposals)
        proposal=db.adaptive.approve_proposal(); self.assertEqual(proposal["status"], "committed")
        self.assertIn("idx_sable_key", db.adaptive.status()["active_indexes"]); db.close()
    def test_query_and_telemetry_export(self):
        db=SableDB(":memory:"); db.execute("SELECT 1 AS x")
        out=Path(tempfile.mktemp()); db.export_telemetry(str(out)); text=out.read_text(); self.assertIn("latency_us", text); self.assertIn("candidate_plans", text); out.unlink(); db.close()

if __name__ == "__main__": unittest.main()
