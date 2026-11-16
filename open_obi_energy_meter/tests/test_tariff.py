"""Exercise the tariff time-window rules (include/tariff_window.h) on the host.

Covers the [start,end) semantics, the unused/wrap conventions, §14a zone resolution at minute
resolution (issue #116), the accepted wire spellings of a window edge, and the whole-hour
approximation kept for older API clients and firmware downgrades.
"""

import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def native(tmp_path_factory):
    binary = tmp_path_factory.mktemp("tariff") / "native"
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error", "-g",
        "-I", str(ROOT / "include"), str(ROOT / "tests/tariff_native.cpp"), "-o", str(binary),
    ], check=True)
    return binary


def test_tariff_window_invariants(native):
    """The assertions live in C++ so they run against the very code the firmware compiles."""
    result = subprocess.run([str(native)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "all invariants hold" in result.stdout
