#!/usr/bin/env python3
"""Functional checks using the user-supplied fixture; never commits the drawing."""
import argparse
from collections import Counter
import hashlib
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile

import ezdxf
from ezdxf import bbox

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--fixture", required=True, type=Path)
parser.add_argument("--frontend", type=Path, default=ROOT / "dist/linux/dwf3d")
parser.add_argument("--writer", type=Path, default=ROOT / "dist/linux/dwgwriter")
args, remaining = parser.parse_known_args()
DATA = args.fixture.read_bytes()
EXPECTED = "4c7e6a63735a2aba5cbdbf3660f2e281cafd1f56f37fafa271fa351f01a2665e"
if hashlib.sha256(DATA).hexdigest() != EXPECTED:
    parser.error("Este teste requer o F-2151C.dwf original validado (SHA-256 diferente).")


class Conversion(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)

    def frontend(self, data=DATA, name="saida.dxf"):
        source = self.path / "entrada.dwf"
        source.write_bytes(data)
        target = self.path / name
        result = subprocess.run([str(args.frontend), str(source), str(target)], capture_output=True, text=True, timeout=120)
        return result, target

    def repack(self, replace):
        output = io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(DATA)) as old, zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as new:
            for item in old.infolist():
                new.writestr(item.filename, replace(item.filename, old.read(item)))
        return b"(DWF V06.01)" + output.getvalue()

    def test_full_conversion_and_dwg_roundtrip(self):
        result, dxf = self.frontend()
        self.assertEqual(result.returncode, 0, result.stderr)
        doc = ezdxf.readfile(dxf)
        audit = doc.audit()
        self.assertEqual(len(audit.errors), 0)
        self.assertEqual(len(audit.fixes), 0)
        self.assertEqual(Counter(e.dxftype() for e in doc.modelspace()), {"3DFACE": 57000, "CIRCLE": 512})
        self.assertEqual(doc.units, 6)
        bounds = bbox.extents(doc.modelspace())
        expected_min = (2203.0634665489197, 2412.3359375, 99.59999692440033)
        expected_max = (2228.0451172590256, 2422.6640625, 103.45000317692757)
        for actual, expected in zip((*bounds.extmin, *bounds.extmax), (*expected_min, *expected_max)):
            self.assertAlmostEqual(actual, expected, places=7)
        dwg = self.path / "resultado.dwg"
        writer = subprocess.run([str(args.writer), str(dxf), str(dwg)], capture_output=True, text=True, timeout=120)
        self.assertEqual(writer.returncode, 0, writer.stderr)
        self.assertIn("57000 faces 3D, 512 circulos", writer.stdout)
        self.assertEqual(dwg.read_bytes()[:6], b"AC1015")
        inspected = subprocess.run([str(args.writer), "--inspect", str(dwg)], capture_output=True, text=True, timeout=30)
        self.assertEqual(inspected.returncode, 0, inspected.stderr)
        before = dwg.read_bytes()
        second = subprocess.run([str(args.writer), str(dxf), str(dwg)], capture_output=True, timeout=30)
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual(dwg.read_bytes(), before)

    def test_existing_dxf_is_preserved(self):
        target = self.path / "saida.dxf"
        target.write_bytes(b"existing user data")
        result, _ = self.frontend()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_bytes(), b"existing user data")

    def test_non_dwf_is_rejected(self):
        result, target = self.frontend(b"not a dwf")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(target.exists())

    def test_truncated_zip_is_rejected(self):
        result, target = self.frontend(DATA[:-25])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(target.exists())

    def test_crc_error_is_rejected(self):
        data = bytearray(DATA)
        pos = data.find(b"PK\x01\x02")
        while pos >= 0:
            n = struct.unpack_from("<H", data, pos + 28)[0]
            if bytes(data[pos + 46:pos + 46 + n]).endswith(b".w3d"):
                data[pos + 16] ^= 1
                break
            pos = data.find(b"PK\x01\x02", pos + 4)
        self.assertGreaterEqual(pos, 0)
        result, target = self.frontend(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CRC", result.stderr)
        self.assertFalse(target.exists())

    def test_unknown_units_are_rejected(self):
        data = self.repack(lambda name, data: data.replace(b'type="m"', b'type="unknown"') if name.endswith("descriptor.xml") else data)
        result, target = self.frontend(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unidade nao suportada", result.stderr)
        self.assertFalse(target.exists())

    def test_unsupported_resource_transform_is_rejected(self):
        data = self.repack(lambda name, data: data.replace(b'transform="1 0 0 0', b'transform="2 0 0 0') if name.endswith("descriptor.xml") else data)
        result, target = self.frontend(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Transformacao de recurso", result.stderr)
        self.assertFalse(target.exists())

    def test_incomplete_w3d_is_rejected(self):
        data = self.repack(lambda name, data: data[:30] if name.endswith(".w3d") else data)
        result, target = self.frontend(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(target.exists())

    def test_external_reference_is_rejected(self):
        # Retain this fixture's valid header/file-info, then insert an external
        # reference opcode with padding, so the parser actually reaches it.
        data = self.repack(lambda name, data: data[:37] + b"\x12" + b"\x00" * 64 if name.endswith(".w3d") else data)
        result, target = self.frontend(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("opcode 18", result.stderr)
        self.assertFalse(target.exists())

    def test_writer_rejects_incomplete_geometry(self):
        dxf=self.path / "incomplete.dxf"
        dxf.write_text("0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1015\n9\n$INSUNITS\n70\n6\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n0\n3DFACE\n10\n1\n0\nENDSEC\n0\nEOF\n")
        dwg=self.path / "incomplete.dwg"
        result=subprocess.run([str(args.writer),str(dxf),str(dwg)],capture_output=True,text=True,timeout=30)
        self.assertNotEqual(result.returncode,0)
        self.assertFalse(dwg.exists())


if __name__ == "__main__":
    unittest.main(argv=[__file__, *remaining], verbosity=2)
