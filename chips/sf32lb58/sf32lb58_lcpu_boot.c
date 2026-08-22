/****************************************************************************
 * chips/sf32lb58/sf32lb58_lcpu_boot.c
 *
 * LCPU boot sequence for SF32LB58 — ported from SF32LB52's
 * sf32lb52_lcpu_boot.c with SF32LB58-specific image and patch references.
 ****************************************************************************/

#include <sfconfig.h>
#include <bf0_hal.h>
#include <nuttx/cache.h>
#include <syslog.h>
#include <string.h>

#include "mem_map.h"
#include "register.h"

/****************************************************************************
 * External Symbols
 ****************************************************************************/

extern void lcpu_img_install(void);
extern void lcpu_patch_install(void);
extern uint16_t LCPU_CONFIG_get_total_size(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_lcpu_rf_cal_disable;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sf32lb58_lcpu_boot_clean_range(uint32_t addr, uint32_t size)
{
  if (size == 0)
    {
      return;
    }

  up_clean_dcache((uintptr_t)addr, (uintptr_t)addr + size);
}

__WEAK void adc_resume(void)
{
}

__WEAK void lcpu_nvds_config(void)
{
}

void lcpu_rom_config_default(void)
{
  uint8_t is_enable_lxt;

  syslog(LOG_INFO, "sf32lb58 lcpu_boot: lcpu_rom_config_default\n");
  uint8_t is_lcpu_rccal = 0;
  uint32_t wdt_staus = 0xFF;
  uint32_t wdt_time = 10;
  uint16_t wdt_clk = 32768;

  if (HAL_LXT_DISABLED())
    {
      is_lcpu_rccal = 1;
    }

  is_enable_lxt = 1 - is_lcpu_rccal;
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_XTAL_ENABLED, &is_enable_lxt, 1);
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_WDT_STATUS, &wdt_staus, 4);
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_WDT_TIME, &wdt_time, 4);
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_WDT_CLK_FEQ, &wdt_clk, 2);
  HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_RC_CAL_IN_L, &is_lcpu_rccal, 1);

  {
    uint32_t tx_queue = HCPU2LCPU_MB_CH1_BUF_START_ADDR;
    hal_lcpu_bluetooth_rom_config_t config = {0};

    config.bit_valid |= 1 << 10 | 1 << 6;
    config.is_fpga = 0;
    config.default_xtal_enabled = is_enable_lxt;
    HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_HCPU_TX_QUEUE, &tx_queue, 4);
    HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_CONFIG, &config, sizeof(config));
  }
}

__WEAK void lcpu_rom_config(void)
{
  lcpu_rom_config_default();
}

static void lcpu_ble_patch_install(void)
{
  memset((void *)0x204F0000, 0, 0x2000);

  /* Install the LCPU ROM patch.  The patch file is selected at build
   * time by silicon revision: lcpu_patch.c for A1, lcpu_patch_a2.c for
   * A2 (see CMakeLists.txt).  Both define the same symbol.
   */
  lcpu_patch_install();

  /* Flush the patch bin/record and the BT EM buffer written above.
   * The LCPU ROM reads these directly from the bus (no cache), so any
   * lines still sitting in the HCPU D-cache would be invisible to it
   * and the patch would silently not install.
   */
  sf32lb58_lcpu_boot_clean_range((uint32_t)LCPU_PATCH_BUF_START_ADDR,
                                 LCPU_PATCH_BUF_SIZE);
  sf32lb58_lcpu_boot_clean_range((uint32_t)LCPU_PATCH_RECORD_ADDR,
                                 LCPU_PATCH_RECORD_SIZE);
  sf32lb58_lcpu_boot_clean_range(0x204F0000, 0x2000);

  syslog(LOG_INFO,
         "sf32lb58 lcpu_boot: chip rev=%u patch CER=0x%lx record=0x%lx\n",
         (unsigned int)__HAL_SYSCFG_GET_REVID(),
         (unsigned long)hwp_patch->CER,
         (unsigned long)*(volatile uint32_t *)LCPU_PATCH_RECORD_ADDR);

  if (g_lcpu_rf_cal_disable == 0)
    {
      extern void bt_rf_cal(void);
      bt_rf_cal();
    }

  adc_resume();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void lcpu_disable_rf_cal(uint8_t is_disable)
{
  g_lcpu_rf_cal_disable = is_disable;
}

uint8_t lcpu_power_on(void)
{
  syslog(LOG_INFO, "sf32lb58 lcpu_boot: start\n");
  HAL_HPAON_WakeCore(CORE_ID_LCPU);
  HAL_RCC_Reset_and_Halt_LCPU(0);

  lcpu_nvds_config();
  syslog(LOG_INFO, "sf32lb58 lcpu_boot: rom_config\n");
  lcpu_rom_config();
  /* syslog(LOG_INFO, "sf32lb58 lcpu_boot: clean config 0x%08x len=%d\n", */
  /*        (unsigned int)LCPU_CONFIG_START_ADDR, */
  /*        (int)LCPU_CONFIG_get_total_size()); */
  /* sf32lb58_lcpu_boot_clean_range(LCPU_CONFIG_START_ADDR, */
  /*                                LCPU_CONFIG_get_total_size()); */

  /* if (HAL_RCC_GetHCLKFreq(CORE_ID_LCPU) > 24000000) */
  /*   { */
  /*     HAL_RCC_LCPU_SetDiv(2, 1, 5); */
  /*   } */

  syslog(LOG_INFO, "sf32lb58 lcpu_boot: installing img\n");
  lcpu_img_install();

  syslog(LOG_INFO, "sf32lb58 lcpu_boot: config start addr 0x%08x\n",
         (unsigned int)HCPU_LCPU_CODE_START_ADDR);
  HAL_LPAON_ConfigStartAddr((uint32_t *)HCPU_LCPU_CODE_START_ADDR);
  lcpu_ble_patch_install();
  HAL_RCC_ReleaseLCPU();
  /* Do NOT call HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST() here.
   * The bus bridge must stay active for ipc_queue_open() in
   * sf32lb58_bt_controller_enable().  It will be cancelled there.
   */
  HAL_Delay_us(5000);
  syslog(LOG_INFO, "sf32lb58 lcpu_boot: done\n");
  return 0;
}

uint8_t lcpu_power_off(void)
{
  HAL_RCC_Reset_and_Halt_LCPU(0);
  return 0;
}
