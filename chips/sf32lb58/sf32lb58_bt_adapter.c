/****************************************************************************
 * chips/sf32lb58/sf32lb58_bt_adapter.c
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
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <nuttx/cache.h>
#include <nuttx/clock.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include "bf0_hal.h"
#include "circular_buf.h"
#include "ipc_hw.h"
#include "ipc_queue.h"
#include "mem_map.h"
#include "sf32lb58_bt_adapter.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SF32LB58_BT_QID          0
#define SF32LB58_BT_TX_BUF_SIZE  HCPU2LCPU_MB_CH1_BUF_SIZE
#define SF32LB58_BT_TX_BUF_ADDR  HCPU2LCPU_MB_CH1_BUF_START_ADDR
#define SF32LB58_BT_TX_BUF_ALIAS HCPU_ADDR_2_LCPU_ADDR( \
                                    HCPU2LCPU_MB_CH1_BUF_START_ADDR)
#define SF32LB58_BT_RX_BUF_ADDR  LCPU_ADDR_2_HCPU_ADDR( \
                                    LCPU2HCPU_MB_CH1_BUF_START_ADDR)
#define SF32LB58_BT_RX_BUF_SIZE  LCPU2HCPU_MB_CH1_BUF_SIZE
#define SF32LB58_BT_RING_DATA_SIZE \
  ((SF32LB58_BT_RX_BUF_SIZE - sizeof(struct circular_buf)) & ~3UL)
#define SF32LB58_BT_NVDS_BUF_START 0x204FFD00
#define SF32LB58_BT_NVDS_BUF_SIZE  0x200
#define SF32LB58_BT_NVDS_PATTERN   0x4e564453
#define SF32LB58_BT_TRACE          0
#define SF32LB58_BT_H4_CMD         0x01

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef enum
{
  SF32LB58_BT_STATUS_IDLE = 0,
  SF32LB58_BT_STATUS_INITED,
  SF32LB58_BT_STATUS_ENABLED,
} sf32lb58_bt_status_t;

struct sf32lb58_bt_env_s
{
  ipc_queue_handle_t ipc_port;
  uint8_t data_buf[SF32LB58_BT_RX_BUF_SIZE];
  sf32lb58_bt_rx_callback_t notify_host;
  struct work_s rx_work;
  bool queue_open;
  bool wake_held;
  volatile bool rx_work_pending;
  bool rx_worker_running;
  uint32_t rx_read_idx_mirror;
  uint32_t rx_count;
};

struct sf32lb58_bt_nvds_mem_init_s
{
  uint32_t pattern;
  uint16_t used_mem;
  uint16_t writting;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct sf32lb58_bt_env_s g_sf32lb58_bt_env;
static sf32lb58_bt_status_t g_sf32lb58_bt_status = SF32LB58_BT_STATUS_IDLE;
static ipc_hw_q_handle_t g_sf32lb58_bt_tx_hw =
{
  .ch_id = SF32LB58_BT_QID / IPC_HW_QUEUE_NUM,
  .q_idx = SF32LB58_BT_QID % IPC_HW_QUEUE_NUM,
};

static const uint8_t g_sf32lb58_bt_nvds_default_rc10k[] =
{
  0x0d, 0x02, 0x64, 0x19, 0x12, 0x01, 0x01, 0x2f,
  0x04, 0x20, 0x00, 0x00, 0x00, 0x01, 0x06, 0x12,
  0x34, 0x56, 0x78, 0xab, 0xcd, 0x15, 0x01, 0x01
};

static const uint8_t g_sf32lb58_bt_nvds_default_lxt32k[] =
{
  0x2f, 0x04, 0x20, 0x00, 0x00, 0x00, 0x01, 0x06,
  0x12, 0x34, 0x56, 0x78, 0xab, 0xcd, 0x15, 0x01,
  0x01
};

/****************************************************************************
 * External Symbols
 ****************************************************************************/

extern uint8_t lcpu_power_on(void);
extern uint8_t lcpu_power_off(void);

/* bt_rf_cal() is provided by the vendor RF calibration library
 * (chips/drivers/cmsis/sf32lb58x/bt_rf_fulcal.c), compiled when
 * CONFIG_UART_BTH4 is enabled.
 */

/* Stubs for builtin apps that are registered but not compiled */

__attribute__((weak))
int adapter_test_main(int argc, char *argv[])
{
  return 0;
}

__attribute__((weak))
int bttool_main(int argc, char *argv[])
{
  return 0;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sf32lb58_bt_clean_nvds_shared(void)
{
  up_clean_dcache((uintptr_t)SF32LB58_BT_NVDS_BUF_START,
                  (uintptr_t)SF32LB58_BT_NVDS_BUF_START + SF32LB58_BT_NVDS_BUF_SIZE);
}

static bool sf32lb58_bt_rx_ring_valid(struct circular_buf *rx_ring)
{
  uint32_t rd_ptr = rx_ring->read_idx_mirror;
  uint32_t wr_ptr = rx_ring->write_idx_mirror;
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  long buf_size = rx_ring->buffer_size;

  return buf_size == (long)SF32LB58_BT_RING_DATA_SIZE &&
         rd_idx <= SF32LB58_BT_RING_DATA_SIZE &&
         wr_idx <= SF32LB58_BT_RING_DATA_SIZE;
}

static bool sf32lb58_bt_rx_ring_ready(const char *tag)
{
  struct circular_buf *rx_ring =
      (struct circular_buf *)SF32LB58_BT_RX_BUF_ADDR;
  uint32_t rd_ptr = rx_ring->read_idx_mirror;
  uint32_t wr_ptr = rx_ring->write_idx_mirror;
  long buf_size = rx_ring->buffer_size;

  if (!sf32lb58_bt_rx_ring_valid(rx_ring))
    {
      syslog(LOG_WARNING,
             "%s rx ring invalid: buf=%ld expected=%lu rd=%08lx wr=%08lx\n",
             tag,
             buf_size,
             (unsigned long)SF32LB58_BT_RING_DATA_SIZE,
             (unsigned long)rd_ptr,
             (unsigned long)wr_ptr);
      return false;
    }

  return true;
}

static int sf32lb58_bt_wait_rx_ring_ready(void)
{
  struct circular_buf *rx_ring =
      (struct circular_buf *)SF32LB58_BT_RX_BUF_ADDR;
  int i;

  syslog(LOG_INFO, "sf32lb58 wait_rx_ring: buf=0x%08x size=%lu\n",
         (unsigned int)SF32LB58_BT_RX_BUF_ADDR,
         (unsigned long)SF32LB58_BT_RX_BUF_SIZE);

  for (i = 0; i < 1000; i++)
    {
      up_invalidate_dcache((uintptr_t)SF32LB58_BT_RX_BUF_ADDR,
                           (uintptr_t)SF32LB58_BT_RX_BUF_ADDR +
                           SF32LB58_BT_RX_BUF_SIZE);

      if (sf32lb58_bt_rx_ring_valid(rx_ring))
        {
          syslog(LOG_INFO, "sf32lb58 wait_rx_ring: ready after %d iters\n", i);
          return OK;
        }

      if (i < 5 || i % 100 == 0)
        {
          syslog(LOG_INFO, "sf32lb58 wait_rx_ring: iter=%d "
                 "buf_size=%ld rd=%08lx wr=%08lx\n",
                 i,
                 rx_ring->buffer_size,
                 (unsigned long)rx_ring->read_idx_mirror,
                 (unsigned long)rx_ring->write_idx_mirror);
        }

      usleep(1000);
    }

  sf32lb58_bt_rx_ring_ready("sf32lb58 wait");
  return -ETIMEDOUT;
}

static void sf32lb58_bt_prepare_stack_nvds(void)
{
  struct sf32lb58_bt_nvds_mem_init_s *nvds;
  const uint8_t *defaults;
  size_t defaults_len;

  if (HAL_LXT_DISABLED())
    {
      defaults = g_sf32lb58_bt_nvds_default_rc10k;
      defaults_len = sizeof(g_sf32lb58_bt_nvds_default_rc10k);
    }
  else
    {
      defaults = g_sf32lb58_bt_nvds_default_lxt32k;
      defaults_len = sizeof(g_sf32lb58_bt_nvds_default_lxt32k);
    }

  HAL_HPAON_WakeCore(CORE_ID_LCPU);

  nvds = (struct sf32lb58_bt_nvds_mem_init_s *)SF32LB58_BT_NVDS_BUF_START;
  memset((void *)SF32LB58_BT_NVDS_BUF_START, 0, SF32LB58_BT_NVDS_BUF_SIZE);
  nvds->pattern = SF32LB58_BT_NVDS_PATTERN;
  nvds->used_mem = defaults_len;
  nvds->writting = 0;
  memcpy((void *)(nvds + 1), defaults, defaults_len);
  sf32lb58_bt_clean_nvds_shared();

  HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
}

static size_t sf32lb58_bt_ring_data_len(uint32_t rd_ptr, uint32_t wr_ptr,
                                        uint32_t buffer_size)
{
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  uint32_t rd_mirror = CB_GET_PTR_MIRROR(rd_ptr);
  uint32_t wr_mirror = CB_GET_PTR_MIRROR(wr_ptr);

  if (rd_idx == wr_idx)
    {
      return rd_mirror == wr_mirror ? 0 : buffer_size;
    }

  if (wr_idx > rd_idx)
    {
      return wr_idx - rd_idx;
    }

  return buffer_size - (rd_idx - wr_idx);
}

static size_t sf32lb58_bt_ring_space_len(uint32_t rd_ptr, uint32_t wr_ptr,
                                         uint32_t buffer_size)
{
  return buffer_size - sf32lb58_bt_ring_data_len(rd_ptr, wr_ptr,
                                                  buffer_size);
}

static uint32_t sf32lb58_bt_ring_advance(uint32_t ptr, size_t len,
                                         uint32_t buffer_size)
{
  uint32_t idx = CB_GET_PTR_IDX(ptr);
  uint32_t mirror = CB_GET_PTR_MIRROR(ptr);

  idx += len;
  if (idx >= buffer_size)
    {
      idx -= buffer_size;
      mirror = ~mirror;
    }

  return CB_MAKE_PTR_IDX_MIRROR(idx, mirror);
}

static size_t sf32lb58_bt_ring_copy(uint8_t *dst,
                                    const struct circular_buf *rx_ring,
                                    uint32_t rd_ptr,
                                    size_t len)
{
  const uint8_t *pool = (const uint8_t *)(rx_ring + 1);
  uint32_t rd_idx = CB_GET_PTR_IDX(rd_ptr);
  size_t tail;

  if (len == 0)
    {
      return 0;
    }

  tail = rx_ring->buffer_size - rd_idx;
  if (tail >= len)
    {
      memcpy(dst, &pool[rd_idx], len);
      return len;
    }

  memcpy(dst, &pool[rd_idx], tail);
  memcpy(&dst[tail], pool, len - tail);
  return len;
}

static size_t sf32lb58_bt_ring_write(struct circular_buf *tx_ring,
                                     const uint8_t *src, size_t len)
{
  uint8_t *pool = (uint8_t *)(tx_ring + 1);
  uint32_t wr_ptr = tx_ring->write_idx_mirror;
  uint32_t wr_idx = CB_GET_PTR_IDX(wr_ptr);
  size_t space;
  size_t tail;

  space = sf32lb58_bt_ring_space_len(tx_ring->read_idx_mirror,
                                     wr_ptr,
                                     tx_ring->buffer_size);
  if (space == 0)
    {
      return 0;
    }

  if (len > space)
    {
      len = space;
    }

  tail = tx_ring->buffer_size - wr_idx;
  if (tail >= len)
    {
      memcpy(&pool[wr_idx], src, len);
      up_clean_dcache((uintptr_t)&pool[wr_idx],
                      (uintptr_t)&pool[wr_idx] + len);
    }
  else
    {
      memcpy(&pool[wr_idx], src, tail);
      memcpy(pool, &src[tail], len - tail);
      up_clean_dcache((uintptr_t)&pool[wr_idx],
                      (uintptr_t)&pool[wr_idx] + tail);
      up_clean_dcache((uintptr_t)pool,
                      (uintptr_t)pool + len - tail);
    }

  tx_ring->write_idx_mirror = sf32lb58_bt_ring_advance(wr_ptr, len,
                                                       tx_ring->buffer_size);
  up_clean_dcache((uintptr_t)tx_ring,
                  (uintptr_t)tx_ring + sizeof(*tx_ring));
  __DSB();

  return len;
}

static size_t sf32lb58_bt_tx_pending(struct circular_buf *tx_ring,
                                     uint32_t *rd_ptr,
                                     uint32_t *wr_ptr)
{
  uint32_t rd;
  uint32_t wr;
  size_t len;

  up_invalidate_dcache((uintptr_t)SF32LB58_BT_TX_BUF_ADDR,
                       (uintptr_t)SF32LB58_BT_TX_BUF_ADDR +
                       sizeof(*tx_ring));

  rd = tx_ring->read_idx_mirror;
  wr = tx_ring->write_idx_mirror;
  len = sf32lb58_bt_ring_data_len(rd, wr, tx_ring->buffer_size);

  if (len > 0 || rd != 0 || wr != 0)
    {
      syslog(LOG_ERR,
             "sf32lb58 tx_pending: rd=%08lx wr=%08lx bsz=%d len=%lu\n",
             (unsigned long)rd, (unsigned long)wr,
             (int)tx_ring->buffer_size, (unsigned long)len);
    }

  if (rd_ptr != NULL)
    {
      *rd_ptr = rd;
    }

  if (wr_ptr != NULL)
    {
      *wr_ptr = wr;
    }

  return len;
}

static void sf32lb58_bt_trigger_tx(void)
{
  __DSB();
  ipc_hw_trigger_interrupt(&g_sf32lb58_bt_tx_hw);
}

static int sf32lb58_bt_wait_tx_idle(struct circular_buf *tx_ring)
{
  uint32_t start_time = HAL_GetTick();
  uint32_t tick_count = 0;
  uint32_t rd_ptr = 0;
  uint32_t wr_ptr = 0;

  while (sf32lb58_bt_tx_pending(tx_ring, &rd_ptr, &wr_ptr) > 0)
    {
      sf32lb58_bt_trigger_tx();

      if (HAL_GetTick() != start_time)
        {
          tick_count++;
          start_time = HAL_GetTick();
        }

      if (tick_count == 1)
        {
          syslog(LOG_INFO,
                 "sf32lb58 bt tx wait: rd=%08lx wr=%08lx ticks=%lu\n",
                 (unsigned long)rd_ptr,
                 (unsigned long)wr_ptr,
                 (unsigned long)tick_count);
        }

      if (tick_count >= 100)
        {
          syslog(LOG_ERR,
                 "sf32lb58 bt tx busy: rd=%08lx wr=%08lx\n",
                 (unsigned long)rd_ptr,
                 (unsigned long)wr_ptr);
          return -ETIMEDOUT;
        }

      usleep(1000);
    }

  return OK;
}

static size_t sf32lb58_bt_tx_chunk_len(const uint8_t *data,
                                       size_t len,
                                       size_t offset)
{
  size_t remaining = len - offset;

  if (offset == 0 && remaining > 1)
    {
      return 1;
    }

  return remaining;
}

static void sf32lb58_bt_rx_worker(FAR void *arg)
{
  struct sf32lb58_bt_env_s *env = arg;

  for (;;)
    {
      irqstate_t flags;
      int empty_retries = 0;

      for (;;)
        {
          size_t size;
          size_t read_len;
          int ret;
          struct circular_buf *rx_ring;
          uint32_t wr_ptr;

          flags = enter_critical_section();

          if (!env->queue_open ||
              env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
            {
              env->rx_work_pending = false;
              env->rx_worker_running = false;
              leave_critical_section(flags);
              return;
            }

          leave_critical_section(flags);

          up_invalidate_dcache((uintptr_t)SF32LB58_BT_RX_BUF_ADDR,
                               (uintptr_t)SF32LB58_BT_RX_BUF_ADDR +
                               SF32LB58_BT_RX_BUF_SIZE);

          flags = enter_critical_section();

          if (!env->queue_open ||
              env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
            {
              env->rx_work_pending = false;
              env->rx_worker_running = false;
              leave_critical_section(flags);
              return;
            }

          rx_ring = (struct circular_buf *)SF32LB58_BT_RX_BUF_ADDR;
          wr_ptr = rx_ring->write_idx_mirror;
          size = sf32lb58_bt_ring_data_len(env->rx_read_idx_mirror,
                                          wr_ptr,
                                          rx_ring->buffer_size);
          if (size == 0)
            {
              leave_critical_section(flags);

              if (empty_retries++ < 20)
                {
                  usleep(1000);
                  continue;
                }

              break;
            }

          empty_retries = 0;

          if (size > sizeof(env->data_buf))
            {
              size = sizeof(env->data_buf);
            }

          read_len = sf32lb58_bt_ring_copy(env->data_buf, rx_ring,
                                           env->rx_read_idx_mirror, size);
          if (read_len == 0)
            {
              leave_critical_section(flags);
              break;
            }

          env->rx_read_idx_mirror = sf32lb58_bt_ring_advance(
              env->rx_read_idx_mirror, read_len, rx_ring->buffer_size);
          rx_ring->read_idx_mirror = env->rx_read_idx_mirror;
          __DSB();

          up_clean_dcache((uintptr_t)SF32LB58_BT_RX_BUF_ADDR,
                          (uintptr_t)SF32LB58_BT_RX_BUF_ADDR +
                          sizeof(*rx_ring));

          env->rx_count++;

          leave_critical_section(flags);

          if (env->notify_host == NULL)
            {
              continue;
            }

          ret = env->notify_host(env->data_buf, read_len);
          if (ret < 0)
            {
              syslog(LOG_ERR, "sf32lb58 bt rx callback: %d\n", ret);
            }
        }

      flags = enter_critical_section();
      if (!env->rx_work_pending)
        {
          env->rx_worker_running = false;
          leave_critical_section(flags);
          break;
        }

      env->rx_work_pending = false;
      leave_critical_section(flags);
    }
}

static int32_t sf32lb58_bt_rx_ind(ipc_queue_handle_t handle, size_t size)
{
  struct sf32lb58_bt_env_s *env = &g_sf32lb58_bt_env;
  irqstate_t flags;
  bool queue_work;

  if (handle != env->ipc_port)
    {
      return -EINVAL;
    }

  if (!env->queue_open)
    {
      return OK;
    }

  flags = enter_critical_section();
  env->rx_work_pending = true;
  queue_work = !env->rx_worker_running && work_available(&env->rx_work);
  if (queue_work)
    {
      env->rx_worker_running = true;
    }
  leave_critical_section(flags);

  if (queue_work)
    {
      int ret;

      ret = work_queue(HPWORK, &env->rx_work, sf32lb58_bt_rx_worker,
                       env, 0);
      if (ret < 0)
        {
          flags = enter_critical_section();
          env->rx_worker_running = false;
          leave_critical_section(flags);

          syslog(LOG_ERR, "sf32lb58 bt queue rx work failed: %d\n", ret);
          return ret;
        }
    }

  return OK;
}

static int sf32lb58_bt_mailbox_init(void)
{
  struct sf32lb58_bt_env_s *env = &g_sf32lb58_bt_env;
  ipc_queue_cfg_t q_cfg;

  memset(&q_cfg, 0, sizeof(q_cfg));
  q_cfg.qid = SF32LB58_BT_QID;
  q_cfg.tx_buf_size = SF32LB58_BT_TX_BUF_SIZE;
  q_cfg.tx_buf_addr = SF32LB58_BT_TX_BUF_ADDR;
  q_cfg.tx_buf_addr_alias = SF32LB58_BT_TX_BUF_ALIAS;
  q_cfg.rx_buf_addr = SF32LB58_BT_RX_BUF_ADDR;
  q_cfg.rx_ind = sf32lb58_bt_rx_ind;

  env->ipc_port = ipc_queue_init(&q_cfg);
  if (env->ipc_port == IPC_QUEUE_INVALID_HANDLE)
    {
      return -ENODEV;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sf32lb58_bt_controller_init(void)
{
  int ret;

  if (g_sf32lb58_bt_status != SF32LB58_BT_STATUS_IDLE)
    {
      return OK;
    }

  memset(&g_sf32lb58_bt_env, 0, sizeof(g_sf32lb58_bt_env));
  ret = sf32lb58_bt_mailbox_init();
  if (ret < 0)
    {
      return ret;
    }

  g_sf32lb58_bt_status = SF32LB58_BT_STATUS_INITED;
  return OK;
}

int sf32lb58_bt_controller_deinit(void)
{
  int ret;

  if (g_sf32lb58_bt_status == SF32LB58_BT_STATUS_ENABLED)
    {
      ret = sf32lb58_bt_controller_disable();
      if (ret < 0)
        {
          return ret;
        }
    }

  if (g_sf32lb58_bt_status != SF32LB58_BT_STATUS_INITED)
    {
      return -EPERM;
    }

  g_sf32lb58_bt_env.queue_open = false;
  g_sf32lb58_bt_env.rx_work_pending = false;
  g_sf32lb58_bt_env.rx_worker_running = false;
  if (!work_available(&g_sf32lb58_bt_env.rx_work))
    {
      ret = work_cancel_sync(HPWORK, &g_sf32lb58_bt_env.rx_work);
      if (ret < 0 && ret != -ENOENT)
        {
          return ret;
        }
    }

  ret = ipc_queue_deinit(g_sf32lb58_bt_env.ipc_port);
  if (ret < 0)
    {
      return ret;
    }

  memset(&g_sf32lb58_bt_env, 0, sizeof(g_sf32lb58_bt_env));
  g_sf32lb58_bt_env.ipc_port = IPC_QUEUE_INVALID_HANDLE;
  g_sf32lb58_bt_status = SF32LB58_BT_STATUS_IDLE;
  return OK;
}

int sf32lb58_hci_register_callback(sf32lb58_bt_rx_callback_t callback)
{
  if (callback == NULL)
    {
      return -EINVAL;
    }

  if (g_sf32lb58_bt_status == SF32LB58_BT_STATUS_IDLE)
    {
      return -EPERM;
    }

  g_sf32lb58_bt_env.notify_host = callback;
  return OK;
}

int sf32lb58_bt_controller_enable(void)
{
  int ret;

  if (g_sf32lb58_bt_status == SF32LB58_BT_STATUS_ENABLED)
    {
      return OK;
    }

  if (g_sf32lb58_bt_status != SF32LB58_BT_STATUS_INITED)
    {
      return -EPERM;
    }

  syslog(LOG_INFO, "sf32lb58 bt: preparing NVDS at 0x%08x size %d\n",
         (unsigned int)SF32LB58_BT_NVDS_BUF_START,
         (int)SF32LB58_BT_NVDS_BUF_SIZE);
  sf32lb58_bt_prepare_stack_nvds();
  HAL_LCPU_ASSERT_INFO_clear();

  /* Wake the LCPU first so the LPSYS bus bridge is powered.
   * This must happen BEFORE lcpu_power_on() because the IPC mailbox
   * CxIER register is on the LPSYS APB bus and only writable when
   * the bus bridge is active.  lcpu_power_on() will reset+halt the
   * LCPU and reconfigure it, so this early wake is just for the bus
   * bridge power domain.
   */

  HAL_HPAON_WakeCore(CORE_ID_LCPU);
  g_sf32lb58_bt_env.wake_held = true;

  /* Open the IPC queue BEFORE powering on the LCPU.
   * The SiFli SDK does this in zbt_config_mailbox (INIT_DEVICE_EXPORT)
   * which runs before ble_power_on.  The LCPU may send data immediately
   * after boot, so the queue must be ready.
   */

  syslog(LOG_INFO, "sf32lb58 bt: opening IPC before LCPU boot\n");
  ret = ipc_queue_open(g_sf32lb58_bt_env.ipc_port);
  syslog(LOG_INFO, "sf32lb58 bt: ipc_queue_open returned %d\n", ret);
  if (ret < 0)
    {
      syslog(LOG_ERR, "sf32lb58 bt: ipc_queue_open failed: %d\n", ret);
      return ret;
    }

  g_sf32lb58_bt_env.queue_open = true;

  syslog(LOG_INFO, "sf32lb58 bt: powering on LCPU\n");
  ret = lcpu_power_on();
  if (ret != 0)
    {
      syslog(LOG_ERR, "sf32lb58 bt: lcpu_power_on failed: %d\n", ret);
      return -EIO;
    }

  syslog(LOG_INFO, "sf32lb58 bt: LCPU powered on, waiting for init\n");

  /* Wait for LCPU to initialize, matching SiFli SDK behavior (1s) */
  {
    volatile uint32_t i;
    for (i = 0; i < 8499000 * 1; i++)
      ;
  }

  /* Log TX ring metadata for diagnostics */
  {
    struct circular_buf *tx_dbg =
        (struct circular_buf *)SF32LB58_BT_TX_BUF_ADDR;
    up_invalidate_dcache((uintptr_t)SF32LB58_BT_TX_BUF_ADDR,
                         (uintptr_t)SF32LB58_BT_TX_BUF_ADDR +
                         sizeof(*tx_dbg));
    syslog(LOG_INFO,
           "sf32lb58 tx ring: buf_size=%d rd=%08lx wr=%08lx "
           "rd_buf=%p wr_buf=%p\n",
           (int)tx_dbg->buffer_size,
           (unsigned long)tx_dbg->read_idx_mirror,
           (unsigned long)tx_dbg->write_idx_mirror,
           tx_dbg->rd_buffer_ptr, tx_dbg->wr_buffer_ptr);
  }

  syslog(LOG_INFO, "sf32lb58 bt: flushing TX ring, then waiting RX ring\n");
  up_clean_dcache((uintptr_t)SF32LB58_BT_TX_BUF_ADDR,
                  (uintptr_t)SF32LB58_BT_TX_BUF_ADDR +
                  SF32LB58_BT_TX_BUF_SIZE);
  __DSB();

  {
    struct circular_buf *rx_ring =
        (struct circular_buf *)SF32LB58_BT_RX_BUF_ADDR;

    ret = sf32lb58_bt_wait_rx_ring_ready();
    if (ret < 0)
      {
        syslog(LOG_ERR, "sf32lb58 rx ring not ready: %d\n", ret);
        ipc_queue_close(g_sf32lb58_bt_env.ipc_port);
        HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
        g_sf32lb58_bt_env.wake_held = false;
        return ret;
      }

    /* Dump boot data from LCPU for diagnostics */
    {
      uint32_t rd = rx_ring->read_idx_mirror;
      uint32_t wr = rx_ring->write_idx_mirror;
      uint32_t rd_idx = CB_GET_PTR_IDX(rd);
      uint32_t wr_idx = CB_GET_PTR_IDX(wr);
      uint32_t boot_len = (wr_idx >= rd_idx) ? (wr_idx - rd_idx) :
                          (rx_ring->buffer_size - (rd_idx - wr_idx));
      uint8_t *pool = (uint8_t *)(rx_ring + 1);
      char hexbuf[96];
      int n;

      syslog(LOG_INFO,
             "sf32lb58 rx boot: rd=%08lx wr=%08lx len=%lu buf_size=%d\n",
             (unsigned long)rd, (unsigned long)wr,
             (unsigned long)boot_len, (int)rx_ring->buffer_size);

      /* Print first64 bytes of boot data as hex */
      n = (boot_len > 64) ? 64 : (int)boot_len;
      if (n > 0)
        {
          int pos = 0;
          int i;
          for (i = 0; i < n && pos < (int)sizeof(hexbuf) - 4; i++)
            {
              snprintf(&hexbuf[pos], sizeof(hexbuf) - pos,
                       "%02x ", pool[(rd_idx + i) % rx_ring->buffer_size]);
              pos += 3;
            }
          hexbuf[pos] = '\0';
          syslog(LOG_INFO, "sf32lb58 rx boot data: %s\n", hexbuf);
        }
    }

    rx_ring->read_idx_mirror = rx_ring->write_idx_mirror;
    g_sf32lb58_bt_env.rx_read_idx_mirror = rx_ring->write_idx_mirror;

    up_clean_dcache((uintptr_t)SF32LB58_BT_RX_BUF_ADDR,
                    (uintptr_t)SF32LB58_BT_RX_BUF_ADDR +
                    sizeof(*rx_ring));
    __DSB();
  }

  g_sf32lb58_bt_status = SF32LB58_BT_STATUS_ENABLED;

  return OK;
}

int sf32lb58_bt_controller_disable(void)
{
  int ret = OK;
  int tmpret;
  bool queue_open;

  if (g_sf32lb58_bt_status != SF32LB58_BT_STATUS_ENABLED)
    {
      return -EPERM;
    }

  queue_open = g_sf32lb58_bt_env.queue_open;
  g_sf32lb58_bt_env.queue_open = false;
  g_sf32lb58_bt_env.rx_work_pending = false;
  g_sf32lb58_bt_env.rx_worker_running = false;
  if (!work_available(&g_sf32lb58_bt_env.rx_work))
    {
      tmpret = work_cancel_sync(HPWORK, &g_sf32lb58_bt_env.rx_work);
      if (tmpret < 0 && tmpret != -ENOENT && ret == OK)
        {
          ret = tmpret;
        }
    }

  if (queue_open)
    {
      tmpret = ipc_queue_close(g_sf32lb58_bt_env.ipc_port);
      if (tmpret < 0 && ret == OK)
        {
          ret = tmpret;
        }
    }

  if (g_sf32lb58_bt_env.wake_held)
    {
      HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
      g_sf32lb58_bt_env.wake_held = false;
    }

  tmpret = lcpu_power_off();
  if (tmpret != 0 && ret == OK)
    {
      ret = -EIO;
    }

  g_sf32lb58_bt_env.notify_host = NULL;
  g_sf32lb58_bt_status = SF32LB58_BT_STATUS_INITED;
  return ret;
}

int sf32lb58_host_send_packet(const uint8_t *data, uint16_t len)
{
  size_t written;

  if (data == NULL || len == 0)
    {
      return -EINVAL;
    }

  if (g_sf32lb58_bt_status != SF32LB58_BT_STATUS_ENABLED ||
      !g_sf32lb58_bt_env.queue_open)
    {
      syslog(LOG_ERR, "sf32lb58 send: not ready status=%d open=%d\n",
             g_sf32lb58_bt_status, g_sf32lb58_bt_env.queue_open);
      return -ENODEV;
    }

  syslog(LOG_ERR,
         "sf32lb58 send: len=%d type=%02x ipc_port=%p\n",
         len, data[0], (void *)(uintptr_t)g_sf32lb58_bt_env.ipc_port);

  /* Use ipc_queue_write — the same path the SiFli SDK uses.
   * This handles D-cache coherency and interrupt triggering internally.
   */
  written = ipc_queue_write(g_sf32lb58_bt_env.ipc_port, data, len, 100);
  syslog(LOG_ERR,
         "sf32lb58 send: ipc_queue_write returned %lu (expected %d)\n",
         (unsigned long)written, (int)len);
  if (written != len)
    {
      syslog(LOG_ERR, "sf32lb58 send: ipc_queue_write wrote %lu/%d\n",
             (unsigned long)written, (int)len);
      return -ETIMEDOUT;
    }

  return OK;
}
