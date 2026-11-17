# SPDX-License-Identifier: Apache-2.0
SDK_DEMO_PATH := $(abspath .)
ifndef BL_SDK_BASE
$(error Set BL_SDK_BASE to the SDK prepared by firmware/bouffalo/tools/prepare_sdk.py)
endif
CHIP := bl618dg
BOARD := bl618dgdk
CPU_ID := ap
CPU_MODEL := b0
BUILD_DIR ?= build
CROSS_COMPILE ?= riscv64-zephyr-elf-
export BL_SDK_BASE
include $(BL_SDK_BASE)/project.build
