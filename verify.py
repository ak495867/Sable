import sys, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from tests.test_sable import SableTests
from tests.test_research import ResearchFeatureTests

suite = unittest.TestSuite(
    [
        unittest.defaultTestLoader.loadTestsFromTestCase(SableTests),
        unittest.defaultTestLoader.loadTestsFromTestCase(ResearchFeatureTests),
    ]
)
result = unittest.TextTestRunner(verbosity=2).run(suite)
if not result.wasSuccessful():
    raise SystemExit(1)
