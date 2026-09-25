import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts/tools'))
from check_command_binding_plan_pair import check


class PairTest(unittest.TestCase):
    def test_exact_requires_executed_reuse(self):
        with tempfile.TemporaryDirectory() as root:
            off, on = [Path(root)/name for name in ('off', 'on')]
            for path, flag in ((off, '0'), (on, '1')):
                (path/'readback').mkdir(parents=True)
                pixels = b'\x00\xff\x42\xff'
                (path/'readback'/'out').write_bytes(pixels)
                record = dict(provider={}, input_components={}, forced_environment={},
                              source_boundary_mode='capture-source', accepted=True,
                              readback_artifacts_valid=True, loaded_images_stable=True,
                              renderer_environment={'DXMT9_COMMAND_BINDING_PLAN_REUSE': flag},
                              result=dict(loaded_images=[], iterations=[dict(
                                  reset_ok=True, warmup=False,
                                  cpu_scope_wall_ns={'encode_chunk': 100},
                                  readback_artifacts={'out': dict(path='out', bytes=4,
                                      sha256=hashlib.sha256(pixels).hexdigest())})]))
                (path/'result.json').write_text(json.dumps(record))
                (path/'provider.stderr').write_text(
                    f'encode_draw_command_binding_plan_builds={flag} '
                    f'encode_draw_command_binding_plan_reuses={flag} '
                    'gpu_command_buffer_errors=0')
            self.assertTrue(check(off, on)['exact'])
            for path, flag in ((off, None), (on, '0')):
                record = json.loads((path/'result.json').read_text())
                record['renderer_environment'] = (
                    {} if flag is None else {'DXMT9_COMMAND_BINDING_PLAN_REUSE': flag})
                (path/'result.json').write_text(json.dumps(record))
                (path/'provider.stderr').write_text(
                    'encode_draw_command_binding_plan_builds=1 '
                    'encode_draw_command_binding_plan_reuses=1 gpu_command_buffer_errors=0')
            self.assertTrue(check(off, on, default=True)['exact'])
            (on/'provider.stderr').write_text(
                'encode_draw_command_binding_plan_builds=1 '
                'encode_draw_command_binding_plan_reuses=0 gpu_command_buffer_errors=0')
            with self.assertRaisesRegex(ValueError, 'vacuous'):
                check(off, on, default=True)

    def test_runtime_selector_retired(self):
        source = (Path(__file__).resolve().parents[2] /
                  'src/dxmt9/dxmt9_draw_encoder_chunk.mm').read_text()
        self.assertNotIn('DXMT9_COMMAND_BINDING_PLAN_REUSE', source)
        self.assertIn('.commandBindingPlan = &commandBindingPlan', source)


if __name__ == '__main__':
    unittest.main()
