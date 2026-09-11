import tempfile, unittest
from pathlib import Path
from sable import (RowStore, ColumnStore, HybridStore, SegmentStore, VectorizedExecutor, JoinExecutor,
                   CompressionCodec, ShadowExecutor, UCB1Bandit, TabularQLearner, SableDB)

class ResearchFeatureTests(unittest.TestCase):
    def setUp(self): self.rows=[{"id":1,"v":10},{"id":2,"v":20},{"id":3,"v":30}]
    def test_storage_modes_and_durable_migration(self):
        self.assertEqual(RowStore(self.rows).scan(["v"]), [{"v":10},{"v":20},{"v":30}])
        self.assertEqual(ColumnStore(self.rows).scan(["v"]), [{"v":10},{"v":20},{"v":30}])
        self.assertEqual(HybridStore(self.rows, ["v"]).scan(["v"]), [{"v":10},{"v":20},{"v":30}])
        root=tempfile.mkdtemp(); store=SegmentStore(root,"row",2); store.append(self.rows); self.assertEqual(store.stats()["rows"],3); self.assertEqual(len(store.scan(["v"])),3); self.assertTrue(store.migrate("column")); self.assertEqual(store.stats()["mode"],"column"); self.assertEqual(store.scan(["v"]), [{"v":10},{"v":20},{"v":30}]); self.assertEqual(SegmentStore(root).stats()["mode"],"column")
    def test_vectorized_and_joins(self):
        v=VectorizedExecutor(batch_size=2); self.assertEqual(v.add([1,2,3],[4,5,6]),[5,7,9]); self.assertEqual(v.filter([1,2,3,4],lambda x:x%2==0),[2,4]); self.assertEqual(v.compare([1,2,3],">",1),[False,True,True])
        left=[{"id":1,"a":"x"},{"id":2,"a":"y"}]; right=[{"id":2,"b":"z"},{"id":1,"b":"w"}]
        expected=JoinExecutor.hash_join(left,right,"id"); self.assertEqual(JoinExecutor.execute(left,right,"id","nested_loop"), expected); self.assertEqual(JoinExecutor.execute(left,right,"id","merge"), expected)
    def test_compression_codecs_roundtrip(self):
        for codec, values in (("none",[1,2,3]),("zlib",["a","b","a"]),("delta",[10,12,15]),("rle",[1,1,1,2,2])): self.assertEqual(CompressionCodec.decode(CompressionCodec.encode(values,codec),codec),values)
        self.assertEqual(CompressionCodec.profile(list(range(100)))["delta"]["roundtrip"],True)
    def test_shadow_bandit_and_rl(self):
        r=ShadowExecutor.compare(lambda:[1,2],lambda:[1,2]); self.assertTrue(r.rows_equal); self.assertGreaterEqual(r.repetitions,1)
        b=UCB1Bandit(["a","b"]); arm=b.select(); b.update(arm,1.0); self.assertIn("trials", b.status()[arm])
        q=TabularQLearner(["left","right"],epsilon=0); a=q.choose(("scan",)); q.update(("scan",),a,1,("scan",)); self.assertIn(("scan",),q.q)
    def test_engine_research_status_and_policy_control(self):
        db=SableDB(":memory:"); status=db.adaptive.status(); self.assertEqual(status["research"]["join_algorithms"],["nested_loop","hash","merge"]); self.assertIn("delta",status["research"]["compression_codecs"]); self.assertEqual(db.adaptive.enable_policy("bandit"),"bandit"); self.assertIn(db.adaptive.choose_join_algorithm(self.rows,self.rows,"id"),["nested_loop","hash","merge"]); db.close()

if __name__ == "__main__": unittest.main()
