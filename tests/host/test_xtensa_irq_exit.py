#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real CMake backport, including SDK upgrades and header scope."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
CALL = '\tcallx4 a7\t\t/* call handler */\n\tmov a2, a6\t\t/* copy return value */\n'
OLD = '.macro CROSS_STACK_CALL\n' + CALL + '''
#ifdef CONFIG_USERSPACE
\trsil a6, XCHAL_NUM_INTLEVELS
\tcall4 swap_tables
#endif
\tretw
.endm
'''
FIXED = OLD.replace('#ifdef CONFIG_USERSPACE\n\trsil a6, XCHAL_NUM_INTLEVELS',
                    '\trsil a6, XCHAL_NUM_INTLEVELS\n#ifdef CONFIG_USERSPACE')


@unittest.skipUnless(shutil.which('cmake') and shutil.which('cc'), 'CMake and cc required')
class InterruptExitTests(unittest.TestCase):
    def configure(self, root, header):
        source = root / 'sdk/arch/xtensa/include/xtensa_asm2.inc.S'
        source.parent.mkdir(parents=True)
        source.write_text(header)
        (root / 'dummy.c').write_text('int dummy;\n')
        (root / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(irq_exit C)
add_library(arch__xtensa__core STATIC dummy.c)
target_include_directories(arch__xtensa__core PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}/sdk/arch/xtensa/include")
set(CONFIG_XTENSA ON)
set(ZEPHYR_BASE "${{CMAKE_CURRENT_SOURCE_DIR}}/sdk")
include("{(REPO / 'cmake/xtensa_irq_exit_compat.cmake').as_posix()}")
get_target_property(paths arch__xtensa__core INCLUDE_DIRECTORIES)
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/includes.txt" "${{paths}}")
''')
        result = subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build')],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(source.read_text(), header, 'SDK source must stay untouched')
        return result

    def test_mask_precedes_stack_return_with_and_without_userspace(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = self.configure(root, OLD)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            generated = root / 'build/papermono_xtensa_compat/xtensa_asm2.inc.S'
            includes = (root / 'build/includes.txt').read_text().split(';')
            self.assertEqual(Path(includes[0]), generated.parent)
            for userspace in (False, True):
                args = ['cc', '-E', '-P', '-x', 'assembler-with-cpp', str(generated)]
                if userspace:
                    args.append('-DCONFIG_USERSPACE=1')
                body = subprocess.check_output(args, text=True, timeout=30)
                self.assertEqual(body.count('rsil a6, XCHAL_NUM_INTLEVELS'), 1)
                self.assertLess(body.index('callx4 a7'), body.index('rsil a6'))
                self.assertLess(body.index('rsil a6'), body.index('retw'))
                self.assertEqual('swap_tables' in body, userspace)

    def test_fixed_sdk_is_used_directly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = self.configure(root, FIXED)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse((root / 'build/papermono_xtensa_compat').exists())

    def test_unknown_sdk_fails_with_review_message(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.configure(Path(tmp), '.macro CROSS_STACK_CALL\nretw\n.endm\n')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Unrecognized Xtensa interrupt exit', result.stderr)


if __name__ == '__main__':
    unittest.main()
