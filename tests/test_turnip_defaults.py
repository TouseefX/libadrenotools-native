"""Regression checks for process-wide automatic Turnip configuration."""
import pathlib
import re
import unittest

SOURCE = (pathlib.Path(__file__).resolve().parents[1] / "src/driver.cpp").read_text()


class TurnipDefaultsTest(unittest.TestCase):
    def test_no_automatic_hardware_overrides(self):
        # Explicit caller-provided environment settings remain supported. Check
        # literal default assignments across the file, not just the constructor.
        for name in (
            "FD_DEV_FEATURES",
            "tu_override_uncached_as_cache_coherent",
            "TU_ROBUST_BUFFER_ACCESS",
            "TU_GMEM_ALLOW_OVERLAP",
            "TU_RENDERPASS_CACHE",
            "TU_OVERRIDE_HEAP_SIZE",
            "TU_DEBUG",
        ):
            with self.subTest(name=name):
                self.assertNotRegex(
                    SOURCE,
                    r'\b(?:set_default_env|setenv)\s*\(\s*"' + re.escape(name) + r'"',
                )

    def test_explicit_environment_api_remains(self):
        self.assertIn('bool adrenotools_set_freedreno_env(', SOURCE)


if __name__ == "__main__":
    unittest.main()
