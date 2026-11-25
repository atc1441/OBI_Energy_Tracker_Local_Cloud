"""Exercise the reader health rules (include/reader_status.h) on the host.

One rule decides what the dashboard banner, /api/readers and the MQTT/Home-Assistant Status entity all
report, so it is worth pinning down: when a reader counts as silent (five missed reports, floored at
15 min and capped at an hour), when a battery is low, the grace period after a gateway restart, and which
problem wins when several apply at once.
"""

import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def native(tmp_path_factory):
    binary = tmp_path_factory.mktemp("reader_status") / "native"
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error", "-g",
        "-I", str(ROOT / "include"), str(ROOT / "tests/reader_status_native.cpp"), "-o", str(binary),
    ], check=True)
    return binary


def test_reader_status_invariants(native):
    """The assertions live in C++ so they run against the very code the firmware compiles."""
    result = subprocess.run([str(native)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "all invariants hold" in result.stdout
