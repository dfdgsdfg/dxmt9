import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class StkcopControls(unittest.TestCase):
    def run_lane(self, cap=None, fail=False, locked=False, originals=True, binary=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / 'experiments/apps_3rd/app-d3d9-stkcop-bench/app'
            (app / 'BenchCfg').mkdir(parents=True)
            (app / '_appdata_').mkdir()
            (app / 'BenchCfg/base.cfg').write_text('snd_targets 32\n')
            (app / 'BenchCfg/rspec_high.ltx').write_text('snd_targets 32\n')
            if originals:
                for name in ('user.ltx', 'test.ltx'):
                    (app / '_appdata_' / name).write_bytes(b'original\r\n')
            lock = app / '.dxmt9-experiment-config-lock'
            if locked:
                lock.mkdir()
            redist = app.parent / 'redist'
            redist.mkdir()
            (redist / 'test.dll').write_bytes(b'native')
            (root / 'prefix/drive_c/windows/syswow64').mkdir(parents=True)
            launcher = root / 'launch.sh'
            launcher.write_bytes((REPO / 'experiments/launchers/app-d3d9-stkcop-bench.sh').read_bytes())
            (root / 'common.sh').write_text('''
exp_repo_root=$FIXTURE_ROOT
exp_require_var() { :; }
exp_stage_dxmt9() { :; }
exp_run_wine_binary() {
  printf '%s\\0' "$DXMT_EXPERIMENT_WORKDIR" "$@" > "$FIXTURE_ROOT/invocation.bin"
  if [[ "$FIXTURE_FAIL" == 1 ]]; then return 7; fi
  printf 'result' > "$exp_repo_root/experiments/apps_3rd/app-d3d9-stkcop-bench/app/_appdata_/Day.result"
}
''')
            env = {k:v for k,v in os.environ.items() if not k.startswith('DXMT')}
            env.update(FIXTURE_ROOT=str(root), FIXTURE_FAIL=str(int(fail)),
                       DXMT_EXPERIMENT_PREFIX=str(root/'prefix'),
                       DXMT_EXPERIMENT_BINARY=binary or str(app/'bin_test/xrEngine.exe'),
                       DXMT_EXPERIMENT_OUTPUT_DIR=str(root/'out'))
            if cap is not None:
                env['DXMT_STKCOP_SND_TARGETS'] = cap
            proc = subprocess.run(['bash', str(launcher)], env=env, capture_output=True, text=True)
            for name in ('user.ltx', 'test.ltx'):
                path = app/'_appdata_'/name
                if originals:
                    self.assertEqual(path.read_bytes(), b'original\r\n', proc.stderr)
                else:
                    self.assertFalse(path.exists(), proc.stderr)
            self.assertEqual(lock.exists(), locked, proc.stderr)
            invocation = root/'invocation.bin'
            if invocation.exists():
                self.assertEqual(invocation.read_bytes().split(b'\0')[:-1], [
                    str(app).encode(), env['DXMT_EXPERIMENT_BINARY'].encode(),
                    b'-ltx', b'test.ltx', b'-silent_error_mode',
                    b'-openautomate', b'dayBenchmark.test',
                ], proc.stderr)
            elif not locked and cap not in ('0', '3', '33', 'abc', '16\nsnd_volume_eff 0'):
                self.fail(f'launcher did not dispatch: {proc.stderr}')
            config = root/'out/stkcop-requested.ltx'
            return proc.returncode, config.read_text() if config.exists() else ''

    def test_cap_and_baseline(self):
        self.assertTrue(self.run_lane('16')[1].endswith('snd_targets 16\n'))
        code, config = self.run_lane()
        self.assertEqual(code, 0)
        self.assertNotIn('snd_targets 16', config)

    def test_failure_and_absence_restore(self):
        self.assertEqual(self.run_lane('16', fail=True)[0], 7)
        self.assertEqual(self.run_lane('16', originals=False)[0], 0)

    def test_invalid_and_locked(self):
        for value in ('0', '3', '33', 'abc', '16\nsnd_volume_eff 0'):
            self.assertEqual(self.run_lane(value)[0], 2)
        self.assertEqual(self.run_lane('16', locked=True)[0], 2)

    def test_binary_override_preserves_workdir_arguments_and_config(self):
        self.assertEqual(self.run_lane(binary='/private diagnostic/bin_test/xrEngine.exe')[0], 0)
        self.assertEqual(self.run_lane(binary='/private diagnostic/bin_test/xrEngine.exe', fail=True)[0], 7)


if __name__ == '__main__':
    unittest.main()
