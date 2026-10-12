"""The runtime header and the generator must agree on the generated-code ABI version."""
from pathlib import Path
import re

from recomp.generate import _RUNTIME_ABI_VERSION

REPO = Path(__file__).resolve().parents[2]


def test_header_abi_version_matches_generator():
    header = (REPO / "include" / "f3rt" / "cpu_abi.h").read_text("utf-8")
    match = re.search(r"^#define\s+F3RT_ABI_VERSION\s+(\d+)u?\b", header, re.MULTILINE)
    assert match, "F3RT_ABI_VERSION not found in include/f3rt/cpu_abi.h"
    assert int(match.group(1)) == _RUNTIME_ABI_VERSION


def test_sound_compiler_uses_generator_abi_version():
    source = (REPO / "recomp" / "compile" / "sound.py").read_text("utf-8")
    assert "_RUNTIME_ABI_VERSION" in source
    assert not re.search(r"F3RT_ABI_VERSION\s*(!=\s*)?\d", source)
