# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_d2d)

zephyr_get(D2D_FW_BIN)

# Default: generated stub. Real firmware is in the Sival drop: -DD2D_FW_BIN=.
if(D2D_FW_BIN)
  set(D2D_FW_BLOB_FILE "${D2D_FW_BIN}")

  if(NOT IS_ABSOLUTE "${D2D_FW_BLOB_FILE}")
    set(D2D_FW_BLOB_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${D2D_FW_BLOB_FILE}")
  endif()

  if(NOT EXISTS "${D2D_FW_BLOB_FILE}")
    message(FATAL_ERROR
            "D2D_FW_BIN names no such file:\n  ${D2D_FW_BLOB_FILE}\n"
            "The image ships in the Sival tarball drop.")
  endif()

  # load_fw() rejects a length that is not a multiple of 4.
  file(SIZE "${D2D_FW_BLOB_FILE}" d2d_fw_blob_size)
  math(EXPR d2d_fw_blob_tail "${d2d_fw_blob_size} % 4")
  if(NOT d2d_fw_blob_tail EQUAL 0)
    message(FATAL_ERROR
            "D2D firmware image is ${d2d_fw_blob_size} bytes, which is not a "
            "multiple of 4 and cannot be loaded:\n  ${D2D_FW_BLOB_FILE}")
  endif()
else()
  set(D2D_FW_STUB_UNIT "TT-D2D-FW-STUB..")
  set(D2D_FW_STUB_REPEAT 16)
  set(D2D_FW_BLOB_FILE "${CMAKE_CURRENT_BINARY_DIR}/d2d_fw_stub.bin")

  string(REPEAT "${D2D_FW_STUB_UNIT}" ${D2D_FW_STUB_REPEAT} d2d_fw_stub_bytes)
  file(WRITE "${D2D_FW_BLOB_FILE}" "${d2d_fw_stub_bytes}")
endif()

# Loader and training are separate images: different tile end-state, one die.
if(CONFIG_TT_D2D_TEST_TRAIN)
  target_sources(app PRIVATE src/train.c)
else()
  target_sources(app PRIVATE src/main.c)
endif()

target_sources(app PRIVATE src/chip_init.c src/d2d_fw_blob.c)

# Byte initialisers for d2d_fw_blob.c; rebuilds when the drop image changes.
set(gen_dir ${ZEPHYR_BINARY_DIR}/misc/generated)
generate_inc_file_for_target(app "${D2D_FW_BLOB_FILE}" ${gen_dir}/d2d_fw.inc)
target_include_directories(app PRIVATE ${gen_dir})

# Stub contents for the loader suite to assert against. Absent with a real image.
if(D2D_FW_STUB_UNIT)
  target_compile_definitions(app PRIVATE
                             D2D_FW_STUB_UNIT="${D2D_FW_STUB_UNIT}"
                             D2D_FW_STUB_REPEAT=${D2D_FW_STUB_REPEAT})
endif()

# Drop basic_init / firewall / clock.
target_link_libraries(app PRIVATE gr_smc)
