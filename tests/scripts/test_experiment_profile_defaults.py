"""Tests for the experiment profile default and its record in run output.

Two contracts are covered:

1. `experiments/launchers/common.sh::exp_resolve_profile_defaults` resolves
   `perf` when neither `DXMT_EXPERIMENT_PROFILE` nor `DXMT_PROFILE` is set.
   `perf` is the measurement-safe configuration (validation layer off, warn
   logging, `WINEDEBUG=-all`); `debug` remains the explicit opt-in for
   diagnosing a wild failure.

2. Every run records the configuration it actually ran under. The launcher
   emits a machine-readable `[experiment] profile: ...` line and
   `run_experiment.py::extract_experiment_profile` parses it back into
   `result.json`'s `profile` object. This is what stops a debug-profile
   measurement from being read as a renderer regression: the artifact says
   what produced it.

Run:
    python3 tests/scripts/test_experiment_profile_defaults.py

Or via meson:
    meson test -C build dxmt9-experiment-profile-defaults
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.run_apps import run_experiment  # noqa: E402

COMMON_SH = REPO_ROOT / "experiments" / "launchers" / "common.sh"
PERF_SUMMARIZER = REPO_ROOT / "scripts" / "tools" / "summarize_3dmark05_perf.py"

# Variables the resolver writes, dumped as `key=value` lines so a subshell run
# can be inspected from Python without sourcing bash state into the test.
_REPORTED_VARS = (
    "EXP_PROFILE_NAME",
    "EXP_PROFILE_SOURCE",
    "EXP_DEFAULT_DXMT_VALIDATE",
    "EXP_DEFAULT_DXMT_LOG_LEVEL",
    "EXP_DEFAULT_DXMT_PERF_COUNTERS",
    "EXP_DEFAULT_DXMT_PERF_COUNTERS_PERIODIC_PRESENTS",
    "EXP_DEFAULT_WINEDEBUG",
    "EXP_DEFAULT_DXMT9_OFFLOAD_COMMIT_REPLAY",
    "EXP_DEFAULT_DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE",
)

_DUMP_SCRIPT = "\n".join(
    [
        f'source "{COMMON_SH}"',
        "exp_resolve_profile_defaults",
        *(f'printf "{name}=%s\\n" "${{{name}-}}"' for name in _REPORTED_VARS),
    ]
)


def resolve_profile(env_overrides: dict[str, str]) -> subprocess.CompletedProcess[str]:
    """Run `exp_resolve_profile_defaults` in a subshell with a clean env."""
    env = {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME": str(Path.home()),
    }
    env.update(env_overrides)
    return subprocess.run(
        ["bash", "-c", _DUMP_SCRIPT],
        text=True,
        capture_output=True,
        check=False,
        env=env,
    )


def resolved_vars(env_overrides: dict[str, str]) -> dict[str, str]:
    completed = resolve_profile(env_overrides)
    assert completed.returncode == 0, completed.stderr
    values: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        key, _, value = line.partition("=")
        values[key] = value
    return values


class ProfileResolutionTests(unittest.TestCase):
    def test_unset_profile_resolves_to_perf(self):
        # The incident this guards: an SFIV benchmark run through its own
        # runner (which sets no profile) silently measured the debug profile.
        values = resolved_vars({})
        self.assertEqual(values["EXP_PROFILE_NAME"], "perf")
        self.assertEqual(values["EXP_PROFILE_SOURCE"], "default")

    def test_default_profile_applies_perf_measurement_defaults(self):
        values = resolved_vars({})
        self.assertEqual(values["EXP_DEFAULT_DXMT_VALIDATE"], "0")
        self.assertEqual(values["EXP_DEFAULT_DXMT_LOG_LEVEL"], "warn")
        self.assertEqual(values["EXP_DEFAULT_WINEDEBUG"], "-all")
        self.assertEqual(values["EXP_DEFAULT_DXMT_PERF_COUNTERS"], "1")
        self.assertEqual(
            values["EXP_DEFAULT_DXMT_PERF_COUNTERS_PERIODIC_PRESENTS"], "60"
        )
        self.assertEqual(values["EXP_DEFAULT_DXMT9_OFFLOAD_COMMIT_REPLAY"], "1")
        self.assertEqual(
            values["EXP_DEFAULT_DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE"], "1"
        )

    def test_explicit_debug_still_selects_debug_defaults(self):
        values = resolved_vars({"DXMT_EXPERIMENT_PROFILE": "debug"})
        self.assertEqual(values["EXP_PROFILE_NAME"], "debug")
        self.assertEqual(values["EXP_PROFILE_SOURCE"], "DXMT_EXPERIMENT_PROFILE")
        self.assertEqual(values["EXP_DEFAULT_DXMT_VALIDATE"], "1")
        self.assertEqual(values["EXP_DEFAULT_DXMT_LOG_LEVEL"], "debug")
        self.assertEqual(values["EXP_DEFAULT_WINEDEBUG"], "")
        self.assertEqual(values["EXP_DEFAULT_DXMT_PERF_COUNTERS"], "")

    def test_secondary_dxmt_profile_variable_still_works(self):
        values = resolved_vars({"DXMT_PROFILE": "debug"})
        self.assertEqual(values["EXP_PROFILE_NAME"], "debug")
        self.assertEqual(values["EXP_PROFILE_SOURCE"], "DXMT_PROFILE")
        self.assertEqual(values["EXP_DEFAULT_DXMT_VALIDATE"], "1")

    def test_experiment_profile_takes_precedence_over_dxmt_profile(self):
        values = resolved_vars(
            {"DXMT_EXPERIMENT_PROFILE": "perf", "DXMT_PROFILE": "debug"}
        )
        self.assertEqual(values["EXP_PROFILE_NAME"], "perf")
        self.assertEqual(values["EXP_PROFILE_SOURCE"], "DXMT_EXPERIMENT_PROFILE")

    def test_empty_profile_value_falls_through_to_the_default(self):
        # `${VAR:-...}` semantics: an exported-but-empty value must not be
        # read as a profile selection.
        values = resolved_vars({"DXMT_EXPERIMENT_PROFILE": "", "DXMT_PROFILE": ""})
        self.assertEqual(values["EXP_PROFILE_NAME"], "perf")
        self.assertEqual(values["EXP_PROFILE_SOURCE"], "default")

    def test_uppercase_profile_value_is_normalised(self):
        values = resolved_vars({"DXMT_EXPERIMENT_PROFILE": "DEBUG"})
        self.assertEqual(values["EXP_PROFILE_NAME"], "debug")
        self.assertEqual(values["EXP_DEFAULT_DXMT_VALIDATE"], "1")

    def test_invalid_profile_exits_2_with_the_existing_message(self):
        completed = resolve_profile({"DXMT_EXPERIMENT_PROFILE": "fast"})
        self.assertEqual(completed.returncode, 2, completed.stdout)
        self.assertIn(
            "invalid DXMT_EXPERIMENT_PROFILE: fast (expected debug or perf)",
            completed.stderr,
        )


class ProfileRecordEmissionTests(unittest.TestCase):
    """The launcher must state the configuration it resolved."""

    def _profile_line(self, env_overrides: dict[str, str]) -> str:
        completed = resolve_profile(env_overrides)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        lines = [
            line
            for line in completed.stderr.splitlines()
            if run_experiment.EXPERIMENT_PROFILE_PATTERN.match(line)
        ]
        self.assertEqual(len(lines), 1, completed.stderr)
        return lines[0]

    def test_default_run_emits_a_parseable_profile_record(self):
        parsed = run_experiment.parse_experiment_profile_line(self._profile_line({}))
        self.assertEqual(parsed["name"], "perf")
        self.assertEqual(parsed["source"], "default")
        self.assertEqual(parsed["validate"], "0")
        self.assertEqual(parsed["log_level"], "warn")
        self.assertEqual(parsed["winedebug"], "-all")
        self.assertEqual(parsed["perf_counters"], "1")

    def test_debug_run_records_the_debug_configuration(self):
        parsed = run_experiment.parse_experiment_profile_line(
            self._profile_line({"DXMT_EXPERIMENT_PROFILE": "debug"})
        )
        self.assertEqual(parsed["name"], "debug")
        self.assertEqual(parsed["source"], "DXMT_EXPERIMENT_PROFILE")
        self.assertEqual(parsed["validate"], "1")
        self.assertEqual(parsed["log_level"], "debug")
        # Empty profile values are recorded as an explicit token so the record
        # never has a hole a reader could mistake for "not reported".
        self.assertEqual(parsed["winedebug"], "unset")
        self.assertEqual(parsed["perf_counters"], "unset")

    def test_individual_env_override_is_recorded_not_the_profile_default(self):
        # `exp_run_wine_binary` honours a caller-set DXMT_LOG_LEVEL over the
        # profile default, so the record must report the effective value.
        parsed = run_experiment.parse_experiment_profile_line(
            self._profile_line({"DXMT_LOG_LEVEL": "trace"})
        )
        self.assertEqual(parsed["name"], "perf")
        self.assertEqual(parsed["log_level"], "trace")


class ProfileExtractionTests(unittest.TestCase):
    """`result.json` must carry the profile the run used."""

    def _log_with(self, body: str) -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "dxmt9.log"
        path.write_text(body, encoding="utf-8")
        return path

    def test_missing_log_reports_unavailable(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        missing = Path(tmp.name) / "absent.log"
        self.assertEqual(
            run_experiment.extract_experiment_profile(missing),
            {"name": "unavailable", "source": "unavailable"},
        )

    def test_log_without_marker_reports_unavailable(self):
        log = self._log_with("[experiment] staging dxmt9 runtime into prefix /x\n")
        self.assertEqual(
            run_experiment.extract_experiment_profile(log),
            {"name": "unavailable", "source": "unavailable"},
        )

    def test_profile_record_is_parsed_from_the_run_log(self):
        log = self._log_with(
            "[experiment] staging dxmt9 runtime into prefix /x\n"
            "[experiment] profile: name=perf source=default validate=0"
            " log_level=warn winedebug=-all perf_counters=1"
            " offload_commit_replay=1 optimize_opaque_depth_index_cache=1\n"
            "[experiment] running C:/x.exe profile=perf\n"
        )
        self.assertEqual(
            run_experiment.extract_experiment_profile(log),
            {
                "name": "perf",
                "source": "default",
                "validate": "0",
                "log_level": "warn",
                "winedebug": "-all",
                "perf_counters": "1",
                "offload_commit_replay": "1",
                "optimize_opaque_depth_index_cache": "1",
            },
        )

    def test_first_emission_wins_when_a_launcher_runs_twice(self):
        log = self._log_with(
            "[experiment] profile: name=perf source=default\n"
            "[experiment] profile: name=debug source=DXMT_EXPERIMENT_PROFILE\n"
        )
        self.assertEqual(
            run_experiment.extract_experiment_profile(log)["name"], "perf"
        )

    def test_values_stay_strings_so_the_record_is_unambiguous(self):
        log = self._log_with(
            "[experiment] profile: name=debug source=DXMT_PROFILE validate=1\n"
        )
        extracted = run_experiment.extract_experiment_profile(log)
        self.assertIsInstance(extracted["validate"], str)
        self.assertEqual(extracted["validate"], "1")


class ProfileSummaryReportingTests(unittest.TestCase):
    """A human reading a perf summary must see which profile produced it."""

    def _summarize(self, result_payload: dict) -> str:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        run_dir = Path(tmp.name)
        run_dir.joinpath("result.json").write_text(
            json.dumps(result_payload), encoding="utf-8"
        )
        completed = subprocess.run(
            [sys.executable, str(PERF_SUMMARIZER), str(run_dir)],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return run_dir.joinpath("3dmark05-perf-summary.md").read_text(encoding="utf-8")

    def test_summary_reports_the_recorded_profile(self):
        summary = self._summarize(
            {
                "status": "pass",
                "profile": {"name": "perf", "source": "default", "log_level": "warn"},
                "dxmt9_perf_counters": {"present_encoded": 4},
            }
        )
        self.assertIn("- Profile: `perf`", summary)
        self.assertIn("source `default`", summary)

    def test_summary_reports_unavailable_when_no_profile_was_recorded(self):
        summary = self._summarize(
            {"status": "pass", "dxmt9_perf_counters": {"present_encoded": 4}}
        )
        self.assertIn("- Profile: `unavailable`", summary)


if __name__ == "__main__":
    unittest.main()
