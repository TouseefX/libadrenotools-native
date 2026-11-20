"""Regression checks for the GPU-generation-specific Turnip defaults."""
import pathlib
import unittest

SOURCE = (pathlib.Path(__file__).resolve().parents[1] / "src/driver.cpp").read_text()


class TurnipWorkaroundTest(unittest.TestCase):
    def test_6xx_default_is_confined_to_its_switch_branch(self):
        switch = SOURCE.split("switch (gen) {", 1)[1].split("return gen;", 1)[0]
        branch = switch.split("case 6:", 1)[1].split("break;", 1)[0]
        self.assertIn('set_default_env("TU_DEBUG", "noconform,noubwc,nolrz")', branch)
        self.assertIn('set_default_env("tu_override_uncached_as_cache_coherent", "true")', branch)
        self.assertEqual(switch.count('noconform,noubwc,nolrz'), 1)
        self.assertIn('set_default_env("TU_DEBUG", "noconform")',
                      switch.split("default:", 1)[1])

    def test_defaults_preserve_explicit_environment(self):
        helper = SOURCE.split('static void set_default_env(', 1)[1].split('}', 1)[0]
        self.assertIn('setenv(name, value, 0)', helper)


if __name__ == "__main__":
    unittest.main()
