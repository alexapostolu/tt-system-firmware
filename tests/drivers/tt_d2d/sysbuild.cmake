# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Sysbuild of this test: add a Keraunos image beside the Mimir one.
# Recurses into the same SOURCE_DIR, so bail if that image already exists.
if(TARGET keraunos_train)
  return()
endif()

# Only from a Mimir SMC board; a Keraunos-only build must not recurse.
if(NOT "${BOARD}${BOARD_QUALIFIERS}" MATCHES "tt_mimir/smc")
  return()
endif()

ExternalZephyrProject_Add(
  APPLICATION keraunos_train
  SOURCE_DIR ${APP_DIR}
  BOARD tt_mmk/tt_keraunos/smc
  BUILD_ONLY 1
)

set_config_bool(${DEFAULT_IMAGE} CONFIG_TT_D2D_TEST_TRAIN 1)
set_config_bool(${DEFAULT_IMAGE} CONFIG_TT_D2D_TEST_TRAIN_KERAUNOS 1)
set_config_bool(keraunos_train CONFIG_TT_D2D_TEST_TRAIN 1)

set(${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE ${APP_DIR}/train.overlay CACHE INTERNAL "")
set(keraunos_train_EXTRA_DTC_OVERLAY_FILE ${APP_DIR}/keraunos_train.overlay CACHE INTERNAL "")

# zephyr_get(D2D_FW_BIN) on a secondary image only sees a prefixed cache entry.
if(D2D_FW_BIN)
  set(${DEFAULT_IMAGE}_D2D_FW_BIN "${D2D_FW_BIN}" CACHE STRING "" FORCE)
  set(keraunos_train_D2D_FW_BIN "${D2D_FW_BIN}" CACHE STRING "" FORCE)
endif()
