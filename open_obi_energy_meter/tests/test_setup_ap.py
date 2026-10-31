import os
import subprocess
from pathlib import Path

INCLUDE = Path(__file__).resolve().parents[1] / "include"

CHECK = r'''
#include <assert.h>
#include "setup_ap.h"
int main() {
  SetupApWindow w;
  assert(w.allowed(0, true, 0));                          // boot window
  assert(w.allowed(OBI_SETUP_AP_MS - 1, true, 0));
  assert(!w.allowed(OBI_SETUP_AP_MS, true, 0));           // expired: WiFi drop keeps AP off
  assert(w.allowed(OBI_SETUP_AP_MS, true, 1));            // client still connected
  assert(w.allowed(OBI_SETUP_AP_MS, false, 0));           // nothing saved yet
  assert(!w.allowed(0xFFFFFFF0u, true, 0));               // millis() wrap can't reopen
  assert(!w.allowed(5, true, 0));
  w.start(0xFFFFFFF0u);                                   // button tap across the wrap
  assert(w.allowed(100, true, 0));
  assert(!w.allowed((uint32_t)(0xFFFFFFF0u + OBI_SETUP_AP_MS), true, 0));
  return 0;
}
'''


def test_setup_ap_window(tmp_path):
    src = tmp_path / "check.cpp"
    src.write_text(CHECK)
    exe = tmp_path / "check"
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(INCLUDE), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
