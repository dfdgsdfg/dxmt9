#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = REPO_ROOT / "scripts" / "codegen" / "gen_wine_bridge.py"

SPEC = importlib.util.spec_from_file_location("gen_wine_bridge", GENERATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
GENERATOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GENERATOR
SPEC.loader.exec_module(GENERATOR)


class BridgeCodegenAbiTest(unittest.TestCase):
    def write_schema(
        self, directory: Path, text: str, filename: str = "schema.h"
    ) -> Path:
        path = directory / filename
        path.write_text(text)
        return path

    def hash_schema(self, path: Path) -> int:
        protos = GENERATOR.collect_prototypes([path])
        records = GENERATOR.collect_record_schemas([path])
        return GENERATOR.compute_bridge_abi_hash(protos, records)

    def test_deterministic_for_identical_schema(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write_schema(
                Path(temporary),
                """
typedef struct D9CTestRecord {
  uint32_t first;
  uint16_t second;
} D9CTestRecord;
void dxmt9c_alpha(D9CDevice *device, D9CTestRecord *output);
""",
            )
            first = self.hash_schema(path)
            second = self.hash_schema(path)
            self.assertEqual(first, second)
            self.assertNotEqual(first, 0)

    def test_prototype_declaration_order_changes_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            first = self.write_schema(
                directory,
                """
typedef struct D9CTestRecord { uint32_t value; } D9CTestRecord;
void dxmt9c_alpha(D9CDevice *device);
void dxmt9c_beta(D9CDevice *device);
""",
            )
            second = self.write_schema(
                directory,
                """
typedef struct D9CTestRecord { uint32_t value; } D9CTestRecord;
void dxmt9c_beta(D9CDevice *device);
void dxmt9c_alpha(D9CDevice *device);
""",
                "reordered.h",
            )
            self.assertNotEqual(self.hash_schema(first), self.hash_schema(second))

    def test_pointed_record_layout_mutation_changes_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            first = self.write_schema(
                directory,
                """
typedef struct D9CTestRecord {
  uint32_t value;
} D9CTestRecord;
void dxmt9c_read(D9CDevice *device, D9CTestRecord *output);
""",
            )
            second = self.write_schema(
                directory,
                """
typedef struct D9CTestRecord {
  uint32_t value;
  uint32_t layout_mutation;
} D9CTestRecord;
void dxmt9c_read(D9CDevice *device, D9CTestRecord *output);
""",
                "mutated.h",
            )
            self.assertNotEqual(self.hash_schema(first), self.hash_schema(second))

    def test_packing_context_mutation_changes_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            first = self.write_schema(
                directory,
                """
#pragma pack(push, 1)
typedef struct D9CTestRecord {
  uint32_t value;
  uint16_t tail;
} D9CTestRecord;
#pragma pack(pop)
void dxmt9c_read(D9CDevice *device, D9CTestRecord *output);
""",
            )
            second = self.write_schema(
                directory,
                """
#pragma pack(push, 2)
typedef struct D9CTestRecord {
  uint32_t value;
  uint16_t tail;
} D9CTestRecord;
#pragma pack(pop)
void dxmt9c_read(D9CDevice *device, D9CTestRecord *output);
""",
                "packed-mutated.h",
            )
            self.assertNotEqual(self.hash_schema(first), self.hash_schema(second))

    def test_array_extent_macro_context_changes_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            first = self.write_schema(
                directory,
                """
#define D9C_TEST_EXTENT 4
typedef struct D9CTestRecord {
  uint32_t values[D9C_TEST_EXTENT];
} D9CTestRecord;
void dxmt9c_read(D9CDevice *device, D9CTestRecord *output);
""",
            )
            second = self.write_schema(
                directory,
                """
#define D9C_TEST_EXTENT 8
typedef struct D9CTestRecord {
  uint32_t values[D9C_TEST_EXTENT];
} D9CTestRecord;
void dxmt9c_read(D9CDevice *device, D9CTestRecord *output);
""",
                "array-mutated.h",
            )
            self.assertNotEqual(self.hash_schema(first), self.hash_schema(second))

    def test_repository_schema_contributes_records_and_opcode_order(self) -> None:
        device_header = GENERATOR.DEVICE_C_HEADER
        unix_schema = REPO_ROOT / "src" / "winemetal" / "winemetal_unix_schema.h"
        protos = GENERATOR.collect_prototypes([unix_schema, device_header])
        records = GENERATOR.collect_record_schemas(
            [unix_schema, device_header, GENERATOR.WINEMETAL_THUNKS_HEADER]
        )
        canonical = GENERATOR.canonicalize_schema(protos, records)

        self.assertEqual(len(protos), 166)
        self.assertEqual(protos[0].name, "dxmt9c_factory_create")
        self.assertEqual(protos[-1].name, "dxmt9c_vdecl_get_declaration")
        self.assertIn("op|ordinal=0|dxmt9c_factory_create|", canonical)
        self.assertIn(
            "op|ordinal=165|dxmt9c_vdecl_get_declaration|", canonical
        )
        record_names = {record.name for record in records}
        self.assertIn("D9CCommandChunk", record_names)
        self.assertIn("D9CWsiSurfaceBinding", record_names)
        self.assertIn("Dxmt9WinemetalCompileShaderParams", record_names)
        self.assertIn("layout-context|", canonical)
        self.assertNotEqual(GENERATOR.compute_bridge_abi_hash(protos, records), 0)

    def test_segmented_transport_uses_custom_wow64_adapter(self) -> None:
        device_header = GENERATOR.DEVICE_C_HEADER
        unix_schema = REPO_ROOT / "src" / "winemetal" / "winemetal_unix_schema.h"
        protos = GENERATOR.collect_prototypes([unix_schema, device_header])
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "dispatch.cpp"
            GENERATOR.write_server_cpp(output, "ops.h", protos)
            generated = output.read_text()
        self.assertIn("thunk_wow64_dxmt9c_device_commit_chunk_segmented", generated)
        self.assertIn("segmentedRoleValid", generated)
        self.assertIn("token.hi != 0u", generated)
        self.assertIn("static_cast<std::uint64_t>(token.lo) + bytes >", generated)
        self.assertIn("(std::uint64_t{1} << 32u)", generated)
        self.assertIn("D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES", generated)
        self.assertIn("ScopedWow64NativePointerAllowance records_allowance", generated)
        self.assertIn("ScopedWow64NativePointerAllowance handles_allowance", generated)
        self.assertIn("ScopedWow64NativePointerAllowance payload_allowance", generated)
        self.assertIn("dxmt9c_device_commit_chunk_segmented(", generated)
        self.assertNotIn("std::vector<std::uint8_t> blob(static_cast<std::size_t>(totalBytes))", generated)

    def test_bridge_classification_covers_segmented_commit(self) -> None:
        source = (REPO_ROOT / "src" / "winemetal" / "winemetal_bridge.cpp").read_text()
        self.assertGreaterEqual(
            source.count("BridgeOpcode::dxmt9c_device_commit_chunk_segmented"), 2
        )


if __name__ == "__main__":
    unittest.main()
