# ##############################################################################
# chips/sf32lb58/CMakeLists.txt
#
# Copyright (C) 2026 Yang Hongbo <yang.hongbo@iotpi.xyz>
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

# Path to the shared SiFli vendor tree (HAL, CMSIS, external).
# NUTTX_DIR = <workspace>/nuttx, so NUTTX_DIR/.. = workspace root.
# We must NOT use relative paths from NUTTX_CHIP_ABS_DIR because CMake
# resolves symlinks, making "../../" point to the wrong location.
set(VENDOR_DIR "${NUTTX_DIR}/../contest2026_162_yaotepai/chips")

# Chip-local sources
set(SRCS sifli_start.c sifli_irq.c sifli_uart.c sifli_lowput.c
         sifli_oneshot.c sf32lb58_allocateheap.c sifli_gpio.c sifli_i2c.c
         sifli_spi.c)

if(CONFIG_ADC)
    list(APPEND SRCS sf32lb_adc.c)
endif()

if(CONFIG_RTC)
    list(APPEND SRCS sf32lb_rtc.c)
endif()

if(CONFIG_WATCHDOG)
    list(APPEND SRCS sf32lb_iwdg.c)
endif()

if(CONFIG_MTD)
    list(APPEND SRCS sf32lb_flash.c)
endif()

if(CONFIG_TIMER)
    list(APPEND SRCS sf32lb_timer.c)
endif()

if(CONFIG_PWM)
    list(APPEND SRCS sf32lb_pwm.c)
endif()

if(CONFIG_SF32LB52_USBDEV)
    list(APPEND SRCS sf32lb_usbdev.c)
endif()

# IPC queue sources (shared driver, sf32lb58x porting layer)
set(IPC_QUEUE_SRCS
    ${NUTTX_CHIP_ABS_DIR}/ipc_queue/common/circular_buf.c
    ${NUTTX_CHIP_ABS_DIR}/ipc_queue/common/ipc_hw.c
    ${NUTTX_CHIP_ABS_DIR}/ipc_queue/common/ipc_queue.c
    ${NUTTX_CHIP_ABS_DIR}/ipc_queue/porting/sf32lb58x/hcpu/ipc_hw_port.c)

# Add all HAL source files from the shared vendor tree
file(GLOB HAL_SRCS ${VENDOR_DIR}/drivers/hal/*.c)

if(CONFIG_UART_BTH4)
    list(APPEND SRCS sf32lb58_bt_adapter.c sf32lb58_bth4.c
                     sf32lb58_lcpu_boot.c lcpu_img_58x_3sco.c)
    list(APPEND HAL_SRCS
         ${VENDOR_DIR}/drivers/cmsis/sf32lb58x/lcpu_patch.c)
endif()

# SF32LB58 exclusions — exclude HAL files incompatible with sf32lb58
list(REMOVE_ITEM HAL_SRCS
    ${VENDOR_DIR}/drivers/hal/bf0_hal_audcodec_m.c)

# System initialization and pin config from CMSIS
set(SYSTEM_SRCS
    ${VENDOR_DIR}/drivers/cmsis/sf32lb58x/Templates/system_bf0_ap.c
    ${VENDOR_DIR}/drivers/cmsis/sf32lb58x/bf0_pin_const.c
    ${VENDOR_DIR}/drivers/cmsis/sf32lb58x/lcpu_config_type.c)

target_sources(arch PRIVATE ${SRCS} ${HAL_SRCS} ${SYSTEM_SRCS} ${IPC_QUEUE_SRCS})

target_compile_definitions(arch PRIVATE SOC_BF0_HCPU
                                        SF32LB58X
                                        LCPU_CONFIG_V2
                                        HAL_TICK_PER_SECOND=1000
                                        USE_HAL_DRIVER
                                        USE_SERIALDRIVER)

# Chip-local includes
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/include)

# Vendor driver includes (shared HAL, CMSIS, external)
target_include_directories(arch PRIVATE ${VENDOR_DIR}/drivers/Include)
target_include_directories(arch PRIVATE ${VENDOR_DIR}/drivers/cmsis/Include)
target_include_directories(arch PRIVATE ${VENDOR_DIR}/drivers/cmsis/sf32lb58x)
target_include_directories(arch PRIVATE ${VENDOR_DIR}/external/CMSIS/Include)
target_include_directories(arch PRIVATE ${VENDOR_DIR}/boards/include/config/sf32lb58x)
target_include_directories(arch PRIVATE ${VENDOR_DIR}/middleware/sifli_matrix)

# Board-level includes
target_include_directories(arch PRIVATE ${NUTTX_BOARD_ABS_DIR}/include)

# IPC queue includes
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue/include)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue/common)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue/porting/os/vela)
target_include_directories(arch PRIVATE ${NUTTX_CHIP_ABS_DIR}/ipc_queue/porting/sf32lb58x/hcpu)

# Suppress warnings for vendor HAL driver files
target_compile_options(arch PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-w>
    $<$<COMPILE_LANGUAGE:CXX>:-w>)
