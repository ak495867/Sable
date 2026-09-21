import os
import tempfile
import unittest
from pathlib import Path
from sable import SableDB


class SableTests(unittest.TestCase):
    def test_put_get_and_persistence(self):
        f = tempfile.NamedTemporaryFile(delete=False)
        f.close()
        try:
            db = SableDB(f.name)
            db.put("a", {"price": 42})
            self.assertEqual(db.get("a"), {"price": 42})
            db.close()
            db = SableDB(f.name)
            self.assertEqual(db.get("a"), {"price": 42})
            db.close()
        finally:
            os.unlink(f.name)

    def test_transaction_rolls_back(self):
        db = SableDB(":memory:")
        with self.assertRaises(ValueError):
            with db.transaction():
                db.put("x", 1)
                raise ValueError("rollback")
        self.assertIsNone(db.get("x"))
        db.close()

    def test_bulk_and_adaptation_proposal_commit(self):
        db = SableDB(":memory:", epoch_size=5)
        self.assertEqual(db.bulk_put((f"k{i}", i) for i in range(20)), 20)
        for i in range(40):
            db.get(f"k{i % 20}")
        self.assertTrue(db.adaptive.proposals)
        proposal = db.adaptive.approve_proposal()
        self.assertEqual(proposal["status"], "committed")
        self.assertIn("idx_sable_key", db.adaptive.status()["active_indexes"])
        db.close()

    def test_query_and_telemetry_export(self):
        db = SableDB(":memory:")
        db.execute("SELECT 1 AS x")
        out = Path(tempfile.mktemp())
        db.export_telemetry(str(out))
        text = out.read_text()
        self.assertIn("latency_us", text)
        self.assertIn("candidate_plans", text)
        out.unlink()
        db.close()

    def test_schema_and_column_stats(self):
        db = SableDB(":memory:")
        # Register schema
        db.register_schema(
            [("price", "REAL"), ("symbol", "TEXT"), ("volume", "INTEGER")]
        )
        status = db.adaptive.status()
        self.assertEqual(status["schema"]["table_name"], "sable_records")
        self.assertIn("price", status["schema"]["columns"])
        # Update column stats
        db.update_stats("price", [10.5, 20.0, 30.5, 15.0, None])
        self.assertEqual(db.adaptive.schema.columns["price"].null_count, 1)
        self.assertEqual(db.adaptive.schema.columns["price"].distinct_count, 4)
        db.close()

    def test_index_benefit_estimation(self):
        db = SableDB(":memory:", epoch_size=5)
        # Simulate some queries to build up workload
        db.put("a", {"price": 100})
        db.put("b", {"price": 200})
        db.put("c", {"price": 300})
        for _ in range(10):
            db.get("a")
        # Estimate benefit for key index (should be high)
        benefit = db.get_index_benefit("key")
        self.assertIn("benefit_score", benefit)
        self.assertIn("estimated_reduction_pct", benefit)
        db.close()

    def test_rollback_last_adaptation(self):
        db = SableDB(":memory:", epoch_size=2)
        # Trigger a proposal by doing writes and reads
        db.bulk_put((f"k{i}", i) for i in range(20))
        for i in range(20):
            db.get(f"k{i}")
        self.assertTrue(len(db.adaptive.proposals) > 0)
        # Approve one
        approved = db.adaptive.approve_proposal()
        self.assertEqual(approved["status"], "committed")
        # Now rollback
        rolled_back = db.rollback_last_adaptation()
        self.assertIsNotNone(rolled_back)
        db.close()

    def test_compression_recommendation(self):
        db = SableDB(":memory:")
        recommendation = db.get_compression_recommendation([1, 1, 1, 2, 2, 3, 3, 3])
        self.assertIn("recommended_codec", recommendation)
        self.assertIn("profiles", recommendation)
        db.close()

    # NEW TESTS FOR AUTONOMOUS FEATURES v3.0
    def test_access_tracker(self):
        db = SableDB(":memory:")
        # Access some data
        db.put("user:1", {"name": "Alice", "age": 30})
        db.get("user:1")
        db.get("user:1")  # Second access

        # Check access tracker
        status = db.access_tracker_status()
        self.assertIn("tracked_tables", status)
        self.assertIn("tier_counts", status)
        self.assertGreaterEqual(status["tier_counts"]["warm"], 0)
        db.close()

    def test_adaptive_storage_manager(self):
        db = SableDB(":memory:")
        # Test storage manager classification
        rows = [{"id": 1, "freq": 0.8}, {"id": 2, "freq": 0.3}, {"id": 3, "freq": 0.05}]
        access_freqs = [0.8, 0.3, 0.05]
        result = db.adaptive.storage_manager.classify(access_freqs, rows)
        self.assertIn("hot", result)
        self.assertIn("warm", result)
        self.assertIn("cold", result)
        self.assertEqual(result["hot"], 1)
        self.assertEqual(result["warm"], 1)
        self.assertEqual(result["cold"], 1)
        db.close()

    def test_cost_model(self):
        db = SableDB(":memory:")
        # Observe some query patterns
        db.adaptive.cost_model.observe(1000, 100, 5000, index_used=True)
        db.adaptive.cost_model.observe(10000, 5000, 50000, index_used=False)

        # Test estimation
        cost_with_index = db.adaptive.cost_model.estimate(1000, index_used=True)
        cost_without_index = db.adaptive.cost_model.estimate(1000, index_used=False)
        self.assertLess(cost_with_index, cost_without_index)  # Index should reduce cost

        status = db.cost_model_status()
        self.assertIn("samples", status)
        self.assertIn("parameters", status)
        db.close()

    def test_adaptive_planner(self):
        db = SableDB(":memory:")
        # Test plan selection
        plan, candidates, cache_hit = db.adaptive.planner.select(
            "SELECT * FROM sable_records WHERE key = 'test'", ()
        )
        self.assertIn(plan, ["scan", "point_lookup", "index_lookup"])
        self.assertIsInstance(candidates, tuple)
        self.assertIsInstance(cache_hit, bool)

        # Observe execution to update cost model
        db.adaptive.planner.observe("scan", 100, 1000)

        status = db.adaptive.planner.status()
        self.assertIn("last_plan", status)
        self.assertIn("cost_model", status)
        db.close()

    def test_adaptive_execution_engine(self):
        db = SableDB(":memory:")
        # Test batch size selection
        batch_size = db.adaptive.execution_engine.choose_batch_size(
            1000, 50000
        )  # High latency -> increase batch
        self.assertGreaterEqual(batch_size, 4096)
        self.assertLessEqual(batch_size, 1048576)

        # Test parallelism selection
        parallelism = db.adaptive.execution_engine.choose_parallelism(10000)
        self.assertGreaterEqual(parallelism, 1)
        self.assertLessEqual(parallelism, db.adaptive.execution_engine.max_parallelism)

        # Test memory selection
        memory = db.adaptive.execution_engine.choose_memory(
            1000, 100
        )  # 1000 rows * 100 bytes/row
        self.assertGreaterEqual(memory, 0)
        self.assertLessEqual(
            memory, db.adaptive.execution_engine.memory_budget_mb * 1024 * 1024
        )

        status = db.execution_status()
        self.assertIn("batch_size", status)
        self.assertIn("max_parallelism", status)
        self.assertIn("current_parallelism", status)
        db.close()

    def test_autonomous_physical_designer(self):
        db = SableDB(":memory:")
        # Test proposal creation
        proposal = db.adaptive.physical_designer.propose(
            "test_type", {"param": "value"}, "test reason"
        )
        self.assertEqual(proposal["type"], "test_type")
        self.assertEqual(proposal["details"]["param"], "value")
        self.assertEqual(proposal["reason"], "test reason")
        self.assertEqual(proposal["status"], "proposed")

        # Test validation (simplified)
        validation_result = db.adaptive.physical_designer.validate(
            proposal, None, None, repetitions=1
        )
        self.assertIn("validated", validation_result)
        self.assertIn("winner", validation_result)
        self.assertIn("repetitions", validation_result)

        status = db.physical_designer_status()
        self.assertIn("auto_apply", status)
        self.assertIn("validation_repetitions", status)
        self.assertIn("proposals", status)
        self.assertIn("validation_results", status)
        db.close()


if __name__ == "__main__":
    unittest.main()
