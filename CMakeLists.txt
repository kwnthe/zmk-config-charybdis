# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
#
# This config repo doubles as a Zephyr module (see zephyr/module.yml) so it can carry
# its own behaviors. Guarded so builds without the behavior node never declare an
# empty library.

if(CONFIG_ZMK_BEHAVIOR_TRACKBALL_CPI)
  zephyr_library_named(charybdis_behaviors)
  zephyr_library_sources(src/behavior_trackball_cpi.c)
  zephyr_include_directories(include)
endif()
