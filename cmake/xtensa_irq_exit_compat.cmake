# SPDX-License-Identifier: Apache-2.0
# Backport the exit-side CROSS_STACK_CALL race fix without changing the SDK.
# Reference: soghoyan/zephyr 2a527c5703d9366efbeba3a84efe45bf8d183b8d.
if(NOT CONFIG_XTENSA)
  return()
endif()

set(papermono_xtensa_header
  "${ZEPHYR_BASE}/arch/xtensa/include/xtensa_asm2.inc.S")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${papermono_xtensa_header}")
file(READ "${papermono_xtensa_header}" papermono_xtensa_source)

set(papermono_irq_call "\tcallx4 a7\t\t/* call handler */\n\tmov a2, a6\t\t/* copy return value */\n")
set(papermono_irq_old "${papermono_irq_call}\n#ifdef CONFIG_USERSPACE\n\trsil a6, XCHAL_NUM_INTLEVELS\n")
string(FIND "${papermono_xtensa_source}" "${papermono_irq_old}" papermono_irq_at)
if(papermono_irq_at EQUAL -1)
  # Newer SDKs may already contain the fix. Verify the mask is between the
  # handler call and the userspace-only block before trusting that case.
  string(REGEX MATCH
    "callx4 a7[^\n]*\n[^#]*rsil a6, XCHAL_NUM_INTLEVELS[^#]*#ifdef CONFIG_USERSPACE"
    papermono_irq_fixed "${papermono_xtensa_source}")
  if(NOT papermono_irq_fixed)
    message(FATAL_ERROR "Unrecognized Xtensa interrupt exit; review CROSS_STACK_CALL compatibility")
  endif()
  return()
endif()

set(papermono_irq_new "${papermono_irq_call}
\t/* Keep the saved thread frame protected while retw changes stacks.
\t * _restore_context restores the interrupted PS after the switch.
\t */
\trsil a6, XCHAL_NUM_INTLEVELS

#ifdef CONFIG_USERSPACE
")
string(REPLACE "${papermono_irq_old}" "${papermono_irq_new}"
  papermono_xtensa_patched "${papermono_xtensa_source}")
set(papermono_xtensa_overlay "${CMAKE_CURRENT_BINARY_DIR}/papermono_xtensa_compat")
file(MAKE_DIRECTORY "${papermono_xtensa_overlay}")
file(CONFIGURE OUTPUT "${papermono_xtensa_overlay}/xtensa_asm2.inc.S"
  CONTENT "${papermono_xtensa_patched}" @ONLY)
target_include_directories(arch__xtensa__core BEFORE PRIVATE
  "${papermono_xtensa_overlay}")
message(STATUS "PaperMono: protecting Xtensa interrupt stack exit")
