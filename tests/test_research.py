import tempfile
import unittest
from pathlib import Path
from sable import (
    RowStore,
    ColumnStore,
    HybridStore,
    SegmentStore,
    VectorizedExecutor,
    JoinExecutor,
    CompressionCodec,
    ShadowExecutor,
    UCB1Bandit,
    TabularQLearner,
    SableDB,
    HotWarmColdStore,
    BufferPolicy,
    SchemaMetadata,
)


class ResearchFeatureTests(unittest.TestCase):
    def setUp(self):
        self.rows = [{"id": 1, "v": 10}, {"id": 2, "v": 20}, {"id": 3, "v": 30}]

    def test_storage_modes_and_durable_migration(self):
        self.assertEqual(
            RowStore(self.rows).scan(["v"]), [{"v": 10}, {"v": 20}, {"v": 30}]
        )
        self.assertEqual(
            ColumnStore(self.rows).scan(["v"]), [{"v": 10}, {"v": 20}, {"v": 30}]
        )
        self.assertEqual(
            HybridStore(self.rows, ["v"]).scan(["v"]), [{"v": 10}, {"v": 20}, {"v": 30}]
        )
        root = tempfile.mkdtemp()
        store = SegmentStore(root, "row", 2)
        store.append(self.rows)
        self.assertEqual(store.stats()["rows"], 3)
        self.assertEqual(len(store.scan(["v"])), 3)
        self.assertTrue(store.migrate("column"))
        self.assertEqual(store.stats()["mode"], "column")
        self.assertEqual(store.scan(["v"]), [{"v": 10}, {"v": 20}, {"v": 30}])
        self.assertEqual(SegmentStore(root).stats()["mode"], "column")

    def test_vectorized_and_joins(self):
        v = VectorizedExecutor(batch_size=2)
        self.assertEqual(v.add([1, 2, 3], [4, 5, 6]), [5, 7, 9])
        self.assertEqual(v.filter([1, 2, 3, 4], lambda x: x % 2 == 0), [2, 4])
        self.assertEqual(v.compare([1, 2, 3], ">", 1), [False, True, True])
        left = [{"id": 1, "a": "x"}, {"id": 2, "a": "y"}]
        right = [{"id": 2, "b": "z"}, {"id": 1, "b": "w"}]
        expected = JoinExecutor.hash_join(left, right, "id")
        self.assertEqual(
            JoinExecutor.execute(left, right, "id", "nested_loop"), expected
        )
        self.assertEqual(JoinExecutor.execute(left, right, "id", "merge"), expected)

    def test_compression_codecs_roundtrip(self):
        for codec, values in (
            ("none", [1, 2, 3]),
            ("zlib", ["a", "b", "a"]),
            ("delta", [10, 12, 15]),
            ("rle", [1, 1, 1, 2, 2]),
        ):
            self.assertEqual(
                CompressionCodec.decode(CompressionCodec.encode(values, codec), codec),
                values,
            )
        self.assertEqual(
            CompressionCodec.profile(list(range(100)))["delta"]["roundtrip"], True
        )

    def test_shadow_bandit_and_rl(self):
        r = ShadowExecutor.compare(lambda: [1, 2], lambda: [1, 2])
        self.assertTrue(r.rows_equal)
        self.assertGreaterEqual(r.repetitions, 1)
        b = UCB1Bandit(["a", "b"])
        arm = b.select()
        b.update(arm, 1.0)
        self.assertIn("trials", b.status()[arm])
        q = TabularQLearner(["left", "right"], epsilon=0)
        a = q.choose(("scan",))
        q.update(("scan",), a, 1, ("scan",))
        self.assertIn(("scan",), q.q)

    def test_engine_research_status_and_policy_control(self):
        db = SableDB(":memory:")
        status = db.adaptive.status()
        self.assertEqual(
            status["research"]["join_algorithms"], ["nested_loop", "hash", "merge"]
        )
        self.assertIn("delta", status["research"]["compression_codecs"])
        self.assertEqual(db.adaptive.enable_policy("bandit"), "bandit")
        self.assertIn(
            db.adaptive.choose_join_algorithm(self.rows, self.rows, "id"),
            ["nested_loop", "hash", "merge"],
        )
        db.close()

    def test_bounded_bandit_with_confidence_intervals(self):
        from sable.core import BoundedUCB1Bandit

        bandit = BoundedUCB1Bandit(
            ["arm1", "arm2"], exploration_cap=2.0, decay_rate=0.99
        )
        arm = bandit.select()
        self.assertIn(arm, ["arm1", "arm2"])
        bandit.update(arm, 1.0)
        ci = bandit.confidence_interval(arm)
        self.assertIsInstance(ci, tuple)
        self.assertEqual(len(ci), 2)
        status = bandit.status()
        self.assertIn("confidence_interval", status[arm])
        self.assertIn("exploration_cap", status)
        self.assertIn("decay_rate", status)

    def test_hot_warm_cold_store(self):
        store = HotWarmColdStore(hot_columns=["price"], cold_compression="zlib")
        self.assertEqual(store.mode, "disabled")
        store.enable_classification()
        self.assertEqual(store.mode, "hot_warm_cold")
        # Classify some data
        rows = [
            {"price": 100, "access_freq": 0.8},
            {"price": 50, "access_freq": 0.2},
            {"price": 200, "access_freq": 0.1},
        ]
        freqs = [r["access_freq"] for r in rows]
        result = store.reclassify(freqs, rows)
        self.assertEqual(result["hot"], 1)
        self.assertEqual(result["warm"], 1)
        self.assertEqual(result["cold"], 1)
        stats = store.stats()
        self.assertIn("hot", stats)
        self.assertIn("warm", stats)
        self.assertIn("cold", stats)

    def test_buffer_policy(self):
        policy = BufferPolicy(strategy="adaptive")
        decision = policy.choose(
            "scans", 0.6
        )  # High scan ratio should favor sequential
        self.assertIn(decision, ["sequential", "random", "adaptive"])
        status = policy.status()
        self.assertEqual(status["strategy"], "adaptive")
        self.assertIn("recent_decisions", status)

    def test_schema_metadata(self):
        schema = SchemaMetadata("test_table")
        schema.add_column("id", "INTEGER", distinct=100, nulls=0)
        schema.add_column("name", "TEXT", distinct=50, nulls=5)
        self.assertEqual(len(schema.columns), 2)
        self.assertEqual(schema.columns["id"].dtype, "INTEGER")
        self.assertEqual(schema.columns["name"].distinct_count, 50)
        schema_dict = schema.to_dict()
        self.assertEqual(schema_dict["table_name"], "test_table")
        self.assertIn("columns", schema_dict)
        restored = SchemaMetadata.from_dict(schema_dict)
        self.assertEqual(restored.table_name, "test_table")

    def test_segment_checksum_detects_corruption(self):
        root = tempfile.mkdtemp()
        store = SegmentStore(root, "row", 2)
        store.append(self.rows)
        segment = Path(root) / "segment-000000.rows"
        segment.write_bytes(segment.read_bytes() + b"corrupt")
        with self.assertRaises(IOError):
            store.scan()
