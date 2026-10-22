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
    def test_radio_isr_yields_do_not_enter_thread_scheduler(self):
        for soc in ("ESP32S3", "ESP32C3"):
            with self.subTest(soc=soc), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                bt = root / "components/bt/controller/esp32c3/bt.c"
                coex = root / f"components/esp_coex/{soc.lower()}/esp_coex_adapter.c"
                for source, name in ((bt, "bt_yield"), (coex, "coex_yield")):
                    source.parent.mkdir(parents=True)
                    source.write_text(f'#include <zephyr/kernel.h>\n'
                                      f'void {name}(void) {{ k_yield(); }}\n')
                (root / "zephyr/sys").mkdir(parents=True)
                (root / "zephyr/kernel.h").write_text("""
#include <stdbool.h>
bool k_is_in_isr(void);
void z_impl_k_yield(void);
static inline void k_yield(void) { z_impl_k_yield(); }
""")
                (root / "zephyr/sys/atomic.h").write_text("""
typedef int atomic_t;
static inline int atomic_inc(atomic_t *v) { return (*v)++; }
""")
                (root / "esp_attr.h").write_text('#define IRAM_ATTR\n')
                (root / "main.c").write_text("""
#include <assert.h>
#include <zephyr/kernel.h>
#ifdef z_impl_k_yield
#error Radio compatibility alias leaked into application code
#endif
static bool in_isr;
static unsigned yields;
bool k_is_in_isr(void) { return in_isr; }
void z_impl_k_yield(void) { assert(!in_isr); ++yields; }
void bt_yield(void);
void coex_yield(void);
int main(void) {
    bt_yield(); coex_yield();
    assert(yields == 2);
    in_isr = true;
    bt_yield(); coex_yield();
    assert(yields == 2);
    in_isr = false;
    k_yield(); bt_yield(); coex_yield();
    assert(yields == 5);
}
""")
                (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.20)
project(radio_compat C)
add_executable(zephyr "{bt.as_posix()}" "{coex.as_posix()}" main.c
    "{(REPO / 'src/papermono_esp_radio_compat.c').as_posix()}")
target_include_directories(zephyr PRIVATE "{root.as_posix()}")
set(CONFIG_BT_ESP32 ON)
set(CONFIG_SOC_SERIES_{soc} ON)
set(ZEPHYR_HAL_ESPRESSIF_MODULE_DIR "{root.as_posix()}")
include("{(REPO / 'cmake/esp32_ble_compat.cmake').as_posix()}")
""")
                for command in (["cmake", "-S", str(root), "-B", str(root / "build")],
                                ["cmake", "--build", str(root / "build")],
                                [str(root / "build/zephyr")]):
                    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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
