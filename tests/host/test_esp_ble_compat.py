#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile the real CMake compatibility rule and check its source scope."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "CMake is required")
class ControllerConfigTests(unittest.TestCase):
    def test_security_value_and_translation_unit_scope(self):
        for soc, security, expected in (("ESP32S3", "ON", 1),
                                        ("ESP32C3", "ON", 1),
                                        ("ESP32C3", "OFF", 0),
                                        ("ESP32S2", "ON", 0)):
            with self.subTest(soc=soc, security=security), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                adapter = root / "components/bt/controller/esp32c3/bt.c"
                adapter.parent.mkdir(parents=True)
                adapter.write_text("""
int encryption_fixes(void) {
#if CONFIG_BT_CTRL_BLE_SECURITY_ENABLE
    return 1;
#else
    return 0;
#endif
}
""")
                (root / "main.c").write_text("""
#ifdef CONFIG_BT_CTRL_BLE_SECURITY_ENABLE
#error Controller compatibility alias leaked to unrelated source
#endif
int encryption_fixes(void);
int main(void) { return encryption_fixes() != EXPECTED; }
""")
                (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.20)
project(controller_compat C)
add_executable(zephyr "{adapter.as_posix()}" main.c)
target_compile_definitions(zephyr PRIVATE EXPECTED={expected})
set(CONFIG_BT_ESP32 ON)
set(CONFIG_SOC_SERIES_{soc} ON)
set(CONFIG_ESP32_BT_CTLR_LE_SECURITY_ENABLE {security})
set(ZEPHYR_HAL_ESPRESSIF_MODULE_DIR "{root.as_posix()}")
include("{(REPO / 'cmake/esp32_ble_compat.cmake').as_posix()}")
""")
                for command in (["cmake", "-S", str(root), "-B", str(root / "build")],
                                ["cmake", "--build", str(root / "build")],
                                [str(root / "build/zephyr")]):
                    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
