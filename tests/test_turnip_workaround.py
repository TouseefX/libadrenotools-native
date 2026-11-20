"""Check the Adreno 6xx workaround is applied before legacy TU_DEBUG defaults."""
import pathlib
import unittest

SOURCE = (pathlib.Path(__file__).resolve().parents[1] / "src/driver.cpp").read_text()


class TurnipWorkaroundTest(unittest.TestCase):
    def test_workaround_precedes_legacy_defaults_in_constructor(self):
        constructor = SOURCE.split("static void global_atomic_init() {", 1)[1]
        self.assertLess(constructor.index("set_adreno_6xx_turnip_default(gpu_model)"),
                        constructor.index("applyTurnipOptimizations()"))


if __name__ == "__main__":
    unittest.main()
