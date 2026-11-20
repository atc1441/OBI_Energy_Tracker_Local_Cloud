"""Exercise the in-RAM history ring (include/hist_ram.h) on the host.

Covers the ring itself (fill, wrap, oldest-first order), keeping several readers apart in one buffer,
clearing one reader's samples, and that a row formats exactly like the line the flash log stores.
"""

import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def native(tmp_path_factory):
    binary = tmp_path_factory.mktemp("hist_ram") / "native"
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error", "-g",
        "-I", str(ROOT / "include"), str(ROOT / "tests/hist_ram_native.cpp"), "-o", str(binary),
    ], check=True)
    return binary


def test_ram_history_invariants(native):
    """The assertions live in C++ so they run against the very code the firmware compiles."""
    result = subprocess.run([str(native)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "all invariants hold" in result.stdout
