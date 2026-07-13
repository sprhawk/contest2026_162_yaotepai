/****************************************************************************
 * chips/sf32lb58/sf32lb58_allocateheap.c
 *
 * Copyright (C) 2026 Yang Hongbo <yang.hongbo@iotpi.xyz>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>

#include <stdbool.h>

#include "chip.h"
#include "arm_internal.h"
#include "bf0_hal.h"

extern void BSP_PIN_Init(void);
extern void BSP_Power_Up(bool is_deep_sleep);
extern void BSP_Board_PreInit(void);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SF32LB58 HCPU SRAM layout (mem_map.h):
 *
 *   RAM0: 128 KB @ 0x20000000
 *   RAM1: 128 KB @ 0x20020000
 *   RAM2: 256 KB @ 0x20040000
 *   RAM3: 512 KB @ 0x20080000  (available in extended config)
 *   RAM4: 512 KB @ 0x20100000  (available in extended config)
 *   RAM5: 512 KB @ 0x20180000  (available in extended config)
 *
 * Default HPSYS_RAM_SIZE = RAM0+RAM1+RAM2 = 512 KB
 * Extended (full chip)   = RAM0..RAM5     = 2048 KB
 *
 * We use the full 2 MB here since the SF32LB58 HCPU has access to all
 * RAM0-RAM5 regions. The linker script places .text/.data/.bss in the
 * lower region; the remainder is available as heap.
 */

#define SRAM_START  0x20000000
#define SRAM_SIZE   0x00200000    /* 2 MB (RAM0-RAM5) */
#define SRAM_END    (SRAM_START + SRAM_SIZE)

/* PSRAM memory configuration for SF32LB58 (MPI1 SBUS)
 *
 * a128r32n1 board: 32 MB PSRAM at 0x60000000.
 * First 2 MB reserved for SDK code/data regions (PSRAM_DATA at 0x60200000).
 * NuttX heap uses the remaining space starting at PSRAM_HEAP_START.
 */

#define PSRAM_START      0x60000000
#define PSRAM_HEAP_START 0x60800000    /* After 4 MB reserved region */
#define PSRAM_SIZE       0x01000000    /* 16 MB total */
#define PSRAM_HEAP_SIZE  (PSRAM_SIZE - (PSRAM_HEAP_START - PSRAM_START))  /* 12 MB for heap */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_BSP_USING_PSRAM
typedef struct
{
    const char *name;
    void *instance;
    uint8_t is_qspi;
    uint16_t msize;  /* size in Mbyte */
    uint32_t base_addr;
} bf0_psram_config_t;

typedef union
{
    FLASH_HandleTypeDef qspi_handle;
#if defined(SF32LB55X) && defined(BSP_USING_PSRAM0)
    PSRAM_HandleTypeDef opi_handle;
#endif
} bf0_psram_handle_t;

const static bf0_psram_config_t bf0_psram_cfg[] =
{
#ifdef CONFIG_BSP_USING_PSRAM1
    {
        .name = "psram1",
        .instance = hwp_qspi1,
        .is_qspi = CONFIG_BSP_QSPI1_MODE,
        .msize = CONFIG_BSP_QSPI1_MEM_SIZE,
        .base_addr = QSPI1_MEM_BASE,
    },
#endif /* CONFIG_BSP_USING_PSRAM1 */
#ifdef CONFIG_BSP_USING_PSRAM2
    {
        .name = "psram2",
        .instance = hwp_qspi2,
        .is_qspi = CONFIG_BSP_QSPI2_MODE,
        .msize = CONFIG_BSP_QSPI2_MEM_SIZE,
        .base_addr = QSPI2_MEM_BASE,
    },
#endif /* CONFIG_BSP_USING_PSRAM2 */
};

static bool g_psram_ready = false;
static bf0_psram_handle_t bf0_psram_handle[sizeof(bf0_psram_cfg) / sizeof(bf0_psram_cfg[0])];

static void sifli_psram_preinit(void)
{
  qspi_configure_t qspi_cfg =
  {
    .Instance = hwp_qspi1,
    .SpiMode  = CONFIG_BSP_QSPI1_MODE,
    .msize    = CONFIG_BSP_QSPI1_MEM_SIZE,
    .base     = QSPI1_MEM_BASE,
  };
  static FLASH_HandleTypeDef psram_handle;

  /* Enable 1.8V LDO required by PSRAM (may not be available on all chips). */

#ifdef PMU_PERI_LDO_1V8
  HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, true, true);
#endif

  /* Use SYSCLK for early boot safety. DLL2 path is enabled later by HAL. */

  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_SYSCLK);

  /* Use configured PSRAM mode directly to avoid early-boot PID dependency. */

  if (qspi_cfg.SpiMode == SPI_MODE_NOR)
    {
      g_psram_ready = false;
      return;
    }

  /* Avoid early power-mode query here; HAL_Init will handle PM state later. */

  psram_handle.wakeup = 0;

  /* Keep divider aligned with existing board implementation. */

  g_psram_ready = (HAL_MPI_PSRAM_Init(&psram_handle, &qspi_cfg, 2) == HAL_OK);
}

void *flash_memset(void *s, int c, long count)
{
    char *xs = (char *)s;

    while (count--)
        *xs++ = c;

    return s;
}

static uint8_t psram_get_default_type(uint32_t mpi_id)
{
    uint8_t psram_type = SPI_MODE_NOR;
#ifdef SOC_SF32LB52X
    uint32_t pid = (hwp_hpsys_cfg->IDR & HPSYS_CFG_IDR_PID_Msk) >> HPSYS_CFG_IDR_PID_Pos;
    pid &= 7;

    switch (pid)
    {
    case 5: //BOOT_PSRAM_APS_16P:
        psram_type = SPI_MODE_PSRAM;         // 16Mb APM QSPI PSRAM
        break;
    case 4: //BOOT_PSRAM_APS_32P:
        psram_type = SPI_MODE_LEGPSRAM;    // 32Mb APM LEGACY PSRAM
        break;
    case 6: //BOOT_PSRAM_WINBOND:                // Winbond HYPERBUS PSRAM
        psram_type = SPI_MODE_HBPSRAM;
        break;
    case 3: // BOOT_PSRAM_APS_64P:
    case 2: //BOOT_PSRAM_APS_128P:
        psram_type = SPI_MODE_OPSRAM;      // 64Mb APM XCELLA PSRAM
        break;
    default:
        psram_type = SPI_MODE_NOR;
        break;
    }
#elif defined(SF32LB56X)
    uint32_t sip1 = BSP_Get_Sip1_Mode();
    uint32_t sip2 = BSP_Get_Sip2_Mode();
    if (mpi_id == 1)
    {
        if (sip1 != 0)  // if chip pass ate, use it to cover local config?
        {
            switch (sip1)
            {
            case SFPIN_SIP1_APM_XCA64:
                psram_type = SPI_MODE_OPSRAM;
                break;
            case SFPIN_SIP1_APM_LEG32:
                psram_type = SPI_MODE_LEGPSRAM;
                break;
            case SFPIN_SIP1_WINB_HYP64:
            case SFPIN_SIP1_WINB_HYP32:
                psram_type = SPI_MODE_HBPSRAM;
                break;
            default:
                psram_type = SPI_MODE_NOR;
                break;
            }
        }
        else
        {
#ifdef PSRAM_BL_MODE
            psram_type = PSRAM_BL_MODE;
#endif
        }
    }
    else if (mpi_id == 2)
    {
        if (sip2 != 0)  // if chip pass ate, use it to cover local config?
        {
            switch (sip2)
            {
            case SFPIN_SIP2_APM_XCA128:
                psram_type = SPI_MODE_OPSRAM;
                break;
            case SFPIN_SIP2_APM_LEG32:
                psram_type = SPI_MODE_LEGPSRAM;
                break;
            default:
                psram_type = SPI_MODE_NOR;
                break;
            }
        }
        else
        {
#ifdef PSRAM_BL_MODE
            psram_type = PSRAM_BL_MODE;
#endif
        }
    }
#endif

    return psram_type;
}

/**
* @brief  psram controller hardware initial.
* @retval 0 if success.
*/
int bsp_psramc_init(void)
{
    HAL_StatusTypeDef res;
    uint32_t i;
    uint32_t psram_num;
    uint8_t idx;
    uint16_t psram_clk_div[] =
    {
#ifdef CONFIG_BSP_USING_PSRAM1
        1,
#endif /* CONFIG_BSP_USING_PSRAM1 */
#ifdef CONFIG_BSP_USING_PSRAM2
        1,
#endif /* CONFIG_BSP_USING_PSRAM2 */
    };

    idx = 0;

#ifdef CONFIG_BSP_USING_PSRAM1
    psram_clk_div[idx] = BSP_GetFlash1DIV();
    idx++;
#endif /* CONFIG_BSP_USING_PSRAM1 */
#ifdef CONFIG_BSP_USING_PSRAM2
    psram_clk_div[idx] = BSP_GetFlash2DIV();
    idx++;
#endif /* CONFIG_BSP_USING_PSRAM2 */

    psram_num = sizeof(bf0_psram_cfg) / sizeof(bf0_psram_cfg[0]);

#ifdef SOC_SF32LB56X
    /* extend MPI1 space to 64Mb for 56x if psram supported */
    hwp_hpsys_cfg->SYSCR |= HPSYS_CFG_SYSCR_REMAP;
#endif

    /* init as 0 in case reach here before bss is initialized */
    flash_memset(&bf0_psram_handle[0], 0, sizeof(bf0_psram_handle));
    for (i = 0; i < psram_num; i++)
    {
        if (bf0_psram_cfg[i].is_qspi)
        {
            FLASH_HandleTypeDef *handle;
            qspi_configure_t qspi_cfg;
            uint8_t mtype;

            flash_memset(&qspi_cfg, 0, sizeof(qspi_configure_t));

            handle = &bf0_psram_handle[i].qspi_handle;

            qspi_cfg.Instance = (MPI_TypeDef *)bf0_psram_cfg[i].instance;
            mtype = bf0_psram_cfg[i].is_qspi;

            if (qspi_cfg.Instance == hwp_qspi1)
                mtype = psram_get_default_type(1);
            else if (qspi_cfg.Instance == hwp_qspi2)
                mtype = psram_get_default_type(2);

            if (mtype != SPI_MODE_NOR)  // use auto detected type
                qspi_cfg.SpiMode = mtype;
            else    // use configure type
                qspi_cfg.SpiMode = bf0_psram_cfg[i].is_qspi;

            qspi_cfg.msize = bf0_psram_cfg[i].msize;
            qspi_cfg.base = bf0_psram_cfg[i].base_addr;
            if (PM_STANDBY_BOOT == SystemPowerOnModeGet())
                handle->wakeup = 1;
            else
                handle->wakeup = 0;

            g_psram_ready = (HAL_MPI_PSRAM_Init(handle, &qspi_cfg, psram_clk_div[i]) == HAL_OK);
        }
    }
    return res;
}
#endif /* #ifdef CONFIG_BSP_USING_PSRAM */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void HAL_MspInit(void)
{
  BSP_PIN_Init();
  BSP_Power_Up(true);
}

void HAL_PreInit(void)
{
  BSP_Board_PreInit();

#ifdef CONFIG_BSP_USING_PSRAM
  HAL_MspInit();
  // sifli_psram_preinit();
  // bsp_psramc_init();
#endif
}

/****************************************************************************
 * Name: up_allocate_heap/up_allocate_kheap
 *
 * Description:
 *   This function will be called to dynamically set aside the heap region.
 *
 *   - For the normal "flat" build, this function returns the size of the
 *     single heap.
 *   - For the protected build (CONFIG_BUILD_PROTECTED=y) with both kernel-
 *     and user-space heaps (CONFIG_MM_KERNEL_HEAP=y), this function
 *     provides the size of the unprotected, user-space heap.
 *   - For the kernel build (CONFIG_BUILD_KERNEL=y), this function provides
 *     the size of the protected, kernel-space heap.
 *
 *   The following memory map is assumed for the flat build:
 *
 *     .data region.  Size determined at link time.
 *     .bss  region  Size determined at link time.
 *     IDLE thread stack.  Size determined by CONFIG_IDLETHREAD_STACKSIZE.
 *     Heap.  Extends to the end of SRAM.
 *
 ****************************************************************************/

void up_allocate_heap(FAR void **heap_start, size_t *heap_size)
{
  /* The heap starts at g_idle_topstack and extends to the end of SRAM */

  *heap_start = (FAR void *)g_idle_topstack;
  *heap_size  = SRAM_END - g_idle_topstack;
}

/******************************************************************************
 * Name: arm_addregion
 *
 * Description:
 *   Memory may be added in non-contiguous chunks.  Additional chunks are
 *   added by calling this function.
 *
 ******************************************************************************/

#if CONFIG_MM_REGIONS > 1
void arm_addregion(void)
{
#ifdef CONFIG_BSP_USING_PSRAM
  if (g_psram_ready)
    {
      kumm_addregion((void *)PSRAM_HEAP_START, PSRAM_HEAP_SIZE);
    }
#endif
}
#endif
