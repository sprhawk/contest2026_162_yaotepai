# SiFli SDK → openvela Linker Script Migration Guide

This document records the changes made when migrating the SF32LB52 linker script from the SiFli SDK (`chips/drivers/cmsis/sf32lb52x/Templates/gcc/hcpu/link.lds`) to the openvela NuttX build (`cmake_out/sf32lb52_devkit_lcd_nsh/ld.script.tmp`). Use this as a reference when porting other SiFli chips (e.g., SF32LB58) to openvela.

---

## 1. Memory Regions

### SDK (6 regions)
```
ROM (rx)  : ORIGIN = CODE_START_ADDR,            LENGTH = CODE_SIZE
RAM (rw)  : ORIGIN = HPSYS_RAM0_BASE,            LENGTH = HCPU_RAM_DATA_SIZE
ROM_EX(rw): ORIGIN = HCPU_RO_DATA_START_ADDR,    LENGTH = HCPU_RO_DATA_SIZE
PSRAM(rw) : ORIGIN = PSRAM_DATA_START_ADDR,       LENGTH = PSRAM_DATA_SIZE
ROM2 (rx) : ORIGIN = HCPU_FLASH2_IMG_START_ADDR,  LENGTH = HCPU_FLASH2_IMG_SIZE
ROM3 (rx) : ORIGIN = HCPU_FLASH2_FONT_START_ADDR, LENGTH = HCPU_FLASH2_FONT_SIZE
```

### openvela (3 regions)
```
flash (rx)  : ORIGIN = 0x12010000,  LENGTH = 16M   /* QSPI2, app starts after bootloader */
sram (rwx)  : ORIGIN = 0x20000000,  LENGTH = 512K
psram (rwx) : ORIGIN = 0x60000000,  LENGTH = 8M
```

### What Changed
- **6 regions → 3 regions.** ROM_EX, ROM2, ROM3 removed (font/image/rodata regions not used by NuttX).
- **Code moves from QSPI1 (`0x10000000`) to QSPI2 (`0x12010000`).** The 64KB bootloader occupies `0x12000000`; app code starts at `0x12010000`.
- **Indirect `__ROM_BASE`/`__RAM_BASE` variables replaced with hardcoded values.** The SDK uses `#include "mem_map.h"` to resolve `CODE_START_ADDR` etc.; openvela hardcodes the values directly in the MEMORY block.
- **PSRAM base shifts from `PSRAM_DATA_START_ADDR` (e.g., `0x60400000`) to `0x60000000`.** The SDK reserves a region at the start of PSRAM for code/data; openvela places PSRAM heap at the start.

---

## 2. Entry Point & Vectors

### SDK
```
ENTRY(Reset_Handler)

.vectors : {
    _stext = ABSOLUTE(.);
    KEEP(*(.vectors));
    . = . + 4;                      /* workaround for .retm_data load address */
} > ROM
```

### openvela
```
EXTERN(_vectors)
__Vectors = _vectors;               /* CMSIS compatibility alias */
ENTRY(_stext)

.text : {
    _stext = ABSOLUTE(.);
    . = ALIGN(512);
    KEEP(*(.vectors))
    . = ALIGN(4);
    *(.text .text.*)
    ...
} > flash
```

### What Changed
- **Entry changed from `Reset_Handler` to `_stext`.** NuttX startup code (`sifli_start.c`) defines `__start()` which sets up `_stext`.
- **`.vectors` merged into `.text`.** No longer a separate output section.
- **+4 padding removed.** The SDK workaround to avoid `.retm_data` load address collision is no longer needed (`.retm_data` is removed).
- **`__Vectors = _vectors` added.** Provides CMSIS `SystemInit()` compatibility.

---

## 3. Sections Removed

### 3.1 `.stack` and `.heap`

```
SDK:
  .stack : { . = . + __STACK_SIZE; } > RAM
  .heap  : { . = . + __HEAP_SIZE;  } > RAM

openvela: (removed entirely)
```

**Why:** NuttX manages stacks per-thread (`CONFIG_IDLETHREAD_STACKSIZE`, `CONFIG_PTHREAD_STACK_DEFAULT`) and heap via `up_allocate_heap()` in `sf32lb58_allocateheap.c`. Explicit stack/heap regions conflict with NuttX's memory model.

---

### 3.2 `.retm_data` (Retention RAM data)

```
SDK:
  .retm_data : {
      *(.*l1_ret_text_*)
      *(.*l1_ret_rodata_*)
      *drv_spi_flash.o     (.text* .rodata*)
      *flash_table.o       (.text* .rodata*)
      *bf0_hal_mpi.o       (.text* .rodata*)
      *bf0_hal_mpi_ex.o    (.text* .rodata*)
      *bf0_hal_mpi_psram.o (.text* .rodata*)
      *flash.o             (.text* .rodata*)
      *drv_psram.o         (.text* .rodata*)
      *context_gcc.o       (.text* .rodata*)
      *drv_common.o        (.text.HAL_GetTick)
      *bf0_hal_rcc.o       (.text* .rodata*)
      *bf0_pm.o            (.text.sifli_light_handler)
      *bf0_pm.o            (.text.sifli_deep_handler)
      *bf0_pm.o            (.text.sifli_standby_handler)
      *bf0_pm.o            (.text.SystemInitFromStandby)
      *.o                  (.text.SystemPowerOnModeGet)
      *bsp_init.o          (.text* .rodata*)
      *bsp_lcd_tp.o        (.text* .rodata*)
      *bsp_pinmux.o        (.text* .rodata*)
      *bsp_power.o         (.text* .rodata*)
      *bf0_hal_gpio.o      (.text* .rodata*)
      *bf0_hal_hpaon.o     (.text* .rodata*)
      *bf0_hal.o           (.text.HAL_Init)
      *.o                  (.text.HAL_Delay_us)
      *.o                  (.text.HAL_Delay_us_)
      *.o                  (.text.HAL_Delay_us2_)
      *.o                  (.text.HAL_MspInit)
      *.o                  (.text.HAL_Delay)
      *bf0_hal_pinmux.o    (.text* .rodata*)
      *bf0_pin_const.o     (.text* .rodata*)
      *drv_common.o        (.text.rt_hw_us_delay)
      *.o                  (.text.rt_memset)
      *rt_memclr*.o        (.text*)
      *memset*.o           (.text*)
      __nor_cfg_db_start__ / __nor_cfg_db_end__
      *.o (.retm_data_*)
  } > RAM AT > ROM

openvela: (removed entirely)
```

**Why:** The `.retm_data` section places critical boot code in retention RAM (survives deep sleep). In openvela, the `.ramfunc` section covers the most critical case (flash erase/write functions that must run from RAM). The BSP init code, HAL init, and power management handlers run from flash (XIP) without issues since they don't reprogram the flash they're executing from.

**Note for SF32LB58:** If boot fails without `.retm_data`, the vendor HAL may need some functions in RAM. Check if `HAL_Init()`, `HAL_MspInit()`, or `BSP_Board_PreInit()` access flash during early boot. If so, add them back to a `.ramfunc` section instead.

---

### 3.3 `.retm_bss` (Retention RAM BSS)

```
SDK:
  .retm_bss : {
      *(.bss.retm_bss_*)
  } > RAM

openvela: (removed)
```

**Why:** No retention BSS needed without `.retm_data`.

---

### 3.4 `.rom_ex` (Extended ROM in SRAM tail)

```
SDK:
  .rom_ex : {
      *(.l1_non_ret_text_*)
      *(.l1_non_ret_rodata_*)
  } > ROM_EX AT > ROM

openvela: (removed, ROM_EX region removed)
```

**Why:** ROM_EX was used to place non-retained code at the end of SRAM (`0x201FAC00`, 19KB). This is a SiFli SDK optimization for code that doesn't need to survive sleep but benefits from SRAM execution speed. Not needed for NuttX.

---

### 3.5 `.RW_IRAM0` (Non-retained SRAM data)

```
SDK:
  .RW_IRAM0 : {
      *(non_ret)
      *(.*l1_non_ret_data_*)
      *(.*l1_non_ret_bss_*)
      #ifndef BSP_USING_PSRAM
      *(.nand_cache)
      *(.*l2_non_ret_data_*)
      *(.*l2_non_ret_bss_*)
      *(.*l2_cache_non_ret_data_*)
      *(.*l2_cache_non_ret_bss_*)
      #endif
  } > RAM

openvela: (removed)
```

**Why:** Non-retained data sections are SDK-specific. NuttX uses standard `.data`/`.bss` sections. If PSRAM is enabled, the L2 cache sections go to `.RW_PSRAM_NON_RET` instead.

---

### 3.6 `.rom2` / `.rom3` (Font/Image flash regions)

```
SDK:
  .rom2 : { *(.ROM1_IMG) *(.ROM3_IMG) } > ROM2
  .rom3 : { *lvsf_font_*(.rodata*) } > ROM3

openvela: (removed, ROM2/ROM3 regions removed)
```

**Why:** Font and image data stored in separate flash partitions. Not used by NuttX (fonts are handled by the application layer).

---

## 4. Sections Added for NuttX

### 4.1 `.ramfunc` (Flash driver in SRAM)

```
openvela:
  .ramfunc : {
      . = ALIGN(4);
      _sramfunc = ABSOLUTE(.);
      *(.ramfunc .ramfunc.*)
      . = ALIGN(4);
      _eramfunc = ABSOLUTE(.);
  } > sram AT > flash

  _siramfunc = LOADADDR(.ramfunc);
```

**Why:** Functions that erase/write flash **must** run from RAM (executing from XIP flash while reprogramming it corrupts instructions). The `SF32LB_FLASH_RAMFUNC` attribute in `sf32lb_flash.c` places flash driver functions here.

**Startup:** `__start()` in `sifli_start.c` copies `.ramfunc` from flash to SRAM:
```c
for (src = (const uint32_t *)&_siramfunc,
     dest = (uint32_t *)&_sramfunc; dest < (uint32_t *)&_eramfunc; )
    *dest++ = *src++;
```

---

### 4.2 `.ram_vectors` (RAM vector table)

```
openvela:
  .ram_vectors : {
      *(.ram_vectors)
  } > sram
```

**Why:** NuttX supports dynamic interrupt vector re-routing via `CONFIG_ARCH_RAMVECTORS`. This section holds the RAM-resident vector table.

---

### 4.3 `.tdata` / `.tbss` (Thread-Local Storage)

```
openvela:
  .tdata : { _stdata = ABSOLUTE(.); *(.tdata .tdata.* .gnu.linkonce.td.*); _etdata = ABSOLUTE(.); } > flash
  .tbss  : { _stbss = ABSOLUTE(.);  *(.tbss .tbss.* .gnu.linkonce.tb.* .tcommon);  _etbss = ABSOLUTE(.); } > flash
```

**Why:** NuttX supports TLS via `CONFIG_TLS_NELEM`. These sections hold thread-local variables.

---

### 4.4 `/DISCARD/` (Build-ID removal)

```
openvela:
  /DISCARD/ : { *(.note.gnu.build-id) }
```

**Why:** GCC may insert a `.note.gnu.build-id` section before `.text`, which would land at the flash base address and displace the vector table. Discarding it ensures vectors are at `0x12010000`.

---

### 4.5 `.gnu.linkonce.*` sections

```
openvela (added to .text, .data, .bss, .ARM.extab, .ARM.exidx):
  *(.gnu.linkonce.t.*)    in .text
  *(.gnu.linkonce.r.*)    in .rodata (via .text)
  *(.gnu.linkonce.d.*)    in .data
  *(.gnu.linkonce.b.*)    in .bss
  *(.gnu.linkonce.armextab.*) in .ARM.extab
  *(.gnu.linkonce.armexidx.*) in .ARM.exidx
```

**Why:** GCC linkonce sections allow symbols to be included in multiple object files but linked only once. Required for proper C++ template instantiation and some GCC builtins.

---

### 4.6 `CONSTRUCTORS` in `.data`

```
openvela:
  .data : { ... CONSTRUCTORS ... } > sram AT > flash
```

**Why:** C++ constructor support. The `CONSTRUCTORS` keyword tells the linker to collect constructor function pointers.

---

## 5. Changes to Existing Sections

### 5.1 `.copy.table` — Entries Removed

```
SDK had 3 entries:
  .data, .retm_data, .rom_ex

openvela has 1 entry:
  .data only
```

**Why:** Only `.data` needs to be copied from flash to SRAM at startup. `.retm_data` and `.rom_ex` are removed.

---

### 5.2 `.zero.table` — Entries Removed

```
SDK had 2 entries:
  .bss, .retm_bss

openvela has 1 entry:
  .bss only
```

**Why:** Only `.bss` needs to be zeroed at startup. `.retm_bss` is removed.

---

### 5.3 `.data` — Content Changes

```
SDK:
  *(.l1_ret_data_*)    ← removed
  *(Jlink_RTT)         ← removed
  __RW_IRAM1_start__   ← removed
  __RW_IRAM1_end__     ← removed

openvela:
  (clean NuttX .data section)
```

**Note for SF32LB58:** `*(.l1_ret_data_*)` was kept in the SF32LB58 script because vendor HAL may place retention data there. If unused, it's harmless (empty section).

---

### 5.4 `.bss` — Content Changes

```
SDK:
  *(.bss) *(.bss.*) *(COMMON) *(.l1_ret_bss_*)
  __bss_end = .;              ← SDK alias
  } > RAM AT > RAM            ← BSS load from RAM (not flash)

openvela:
  *(.bss) *(.bss.*) *(.gnu.linkonce.b.*) *(COMMON)
  } > sram                    ← no AT> (BSS is zero-initialized, not copied)
```

**Key change:** SDK has `> RAM AT > RAM` for .bss (load address = runtime address). openvela has just `> sram` (no load address, zero-initialized by startup code).

---

### 5.5 `.text` — Content Removed

```
SDK had (removed in openvela):
  BuiltinApp1Tab_start/end      ← NuttX uses different app registration
  BuiltinApp2Tab_start/end      ← same
  __switch_anim_start__         ← display feature, not needed
  __app_font_start__            ← display feature, not needed
  __SerialTranExport_start__    ← debug feature, not needed
  __nand_cfg_db_start__         ← kept in SF32LB58 for 128MB NAND
```

---

### 5.6 `.ARM.extab` / `.ARM.exidx` — Simplified

```
SDK:
  *(.ARM.extab* .gnu.linkonce.armextab.*)
  *(.ARM.exidx* .gnu.linkonce.armexidx.*)

openvela:
  *(.ARM.extab*)               ← simpler glob
  *(.ARM.exidx*)               ← simpler glob
```

---

## 6. Build-System Guards Removed

The SDK linker script uses C preprocessor guards that are artifacts of the SiFli build system:

```
Removed Guard                    Purpose (SDK)                    Why Removed
───────────────────────────────  ───────────────────────────────  ──────────────────────────
#ifndef FLASH_TABLE_ONLY          Selects flash table vs app code  openvela always builds app
#ifdef FLASH_TABLE_ONLY           Builds flash table only          Not applicable
#ifndef CFG_BOOTLOADER            Selects bootloader vs app        openvela has no bootloader in link
#ifdef ZBT / #include zbt_*.lds   Zero-boot-time ROM patches       Not used
#ifdef BSP_USING_PSRAM            Conditional PSRAM sections       PSRAM always present on target
#ifdef BSP_QSPI2_DUAL_MODE        Dual flash mode DMA code         Not used
```

---

## 7. SF32LB58-Specific Additions

When applying this migration to SF32LB58, the following sections were added (not in SF32LB52):

```
Section                  Region  Purpose
───────────────────────  ──────  ──────────────────────────────────────────
.RW_PSRAM1               psram   Retained L2 cache data (32MB PSRAM)
.RW_PSRAM_NON_RET        psram   Non-retained cache-aligned data (32-byte aligned)
__usbh_class_info_*       .text   USB host class driver registration
__nand_cfg_db_*           .text   128MB NAND flash configuration database
```

These are present in the SF32LB58 SDK template but absent from SF32LB52 because SF32LB58 has larger PSRAM (32MB vs 2MB), USB host support, and NAND flash.

---

## 8. Checklist for Porting a New SiFli Chip

When migrating a new SiFli chip to openvela, follow this checklist:

- [ ] **MEMORY block:** Reduce to 3 regions (flash, sram, psram). Remove ROM_EX, ROM2, ROM3.
- [ ] **Code origin:** Set flash ORIGIN to QSPI2 app start (typically `0x12010000` after bootloader).
- [ ] **SRAM size:** Set to actual RAM0-RAMN total available to HCPU.
- [ ] **PSRAM size:** Set to actual PSRAM size; adjust ORIGIN if first N MB are reserved.
- [ ] **Entry:** Change to `ENTRY(_stext)`, add `EXTERN(_vectors)` and `__Vectors = _vectors`.
- [ ] **Vectors:** Merge `.vectors` into `.text` with `. = ALIGN(512)`.
- [ ] **Remove:** `.stack`, `.heap`, `.retm_data`, `.retm_bss`, `.rom_ex`, `.RW_IRAM0`, `.rom2`, `.rom3`.
- [ ] **Add:** `.ramfunc` in sram AT> flash, `.ram_vectors`, `.tdata`, `.tbss`, `/DISCARD/ .note.gnu.build-id`.
- [ ] **Add to .text:** `__sifli_reg_*`, `__bt_sifli_reg_*`, `__usbh_class_info_*`, `__nand_cfg_db_*`.
- [ ] **Add to .data:** `*(.l1_ret_data_*)`, `CONSTRUCTORS`, `.gnu.linkonce.d.*`.
- [ ] **Add to .bss:** `.gnu.linkonce.b.*`.
- [ ] **Clean .copy.table:** Only `.data` entry.
- [ ] **Clean .zero.table:** Only `.bss` entry.
- [ ] **Remove all `#ifdef` guards** (FLASH_TABLE_ONLY, ZBT, BSP_USING_PSRAM, etc.).
- [ ] **If PSRAM > 8MB:** Add `.RW_PSRAM1` and `.RW_PSRAM_NON_RET` sections.
- [ ] **If NAND flash:** Add `__nand_cfg_db_*` to `.text`.
- [ ] **Test:** Build, verify no unplaced sections, GDB break at `__start`, check .data/.bss/.ramfunc in SRAM.

---

## 9. SiFli-SDK Build System & Firmware Packaging

This section documents how the SiFli-SDK builds firmware, the flash table (ftab) format, the bootloader loading sequence, and how to integrate firmware packaging into the openvela build.

### 9.1 SDK Build Flow (SCons)

The SiFli-SDK uses **SCons** (Python-based build tool) with RT-Thread conventions. The build flow for a project like `example/get-started/hello_world/rtt/project/`:

```
SConstruct
  │
  ├─ PrepareEnv()          → resolve board name, load Kconfig/rtconfig
  ├─ AddBootLoader()       → add bootloader as child project
  ├─ SifliEnv()            → detect toolchain (gcc/keil/iar), set CPU flags
  ├─ PrepareBuilding()     → create SCons Environment, process SConscript groups
  ├─ DoBuilding(TARGET)    → link ELF
  │    └─ EndBuilding()
  │         ├─ ProgramBinary()   → objcopy -Obinary → .bin
  │         ├─ ProgramHex()      → objcopy -Oihex → .hex
  │         ├─ ProgramAsm()      → objdump -d → .asm
  │         └─ LdsFile()         → preprocess linker script
  ├─ AddFTAB()             → generate flash table binary (ftab.bin)
  └─ GenDownloadScript()   → generate JLink download scripts
```

**Key source files:**

| File | Purpose |
|------|---------|
| `tools/build/building.py` | Core build orchestration (4843 lines). Registers builders for ProgramBinary, FtabBin, LdsFile, etc. |
| `tools/build/ptab.py` | Partition table parser (JSON v1/v2, YAML v3). Resolves chip memory maps. |
| `tools/build/gen_ftab.py` | Generates `ftab.bin` binary from ptab v3 YAML partition definitions. |
| `tools/build/gen_link_lds.py` | Renders Jinja2 linker script templates for ptab v3 projects. |
| `tools/build/sdk_resource.py` | Generates `ftab.c` from ptab JSON (v1/v2 flow). |
| `tools/secureboot/dfu_bin_generate37.py` | DFU OTA package generator (AES-256 + RSA-2048). |
| `tools/patch/gen_src.py` | Converts LCPU/ACPU binaries into C source for embedding in HCPU image. |

### 9.2 Linker Script Generation

The SDK supports two linker script generation methods:

**ptab v1/v2 (legacy):** C preprocessor expansion of `.lds` files with `#include "rtconfig.h"` and `#include "mem_map.h"`:
```bash
gcc -E -P -x c link.lds -I<board_dir> -I<chip_dir> -o ld.script.tmp
```

**ptab v3 (modern):** Jinja2 template rendering via `gen_link_lds.py`:
```bash
python gen_link_lds.py --ptab ptab.yaml --template link.jinja2 --output ld.script
```

The Jinja2 templates use partition table data to resolve memory addresses, replacing the `#include "mem_map.h"` approach.

### 9.3 Flash Table (ftab) Format

The SiFli SDK does **not** prepend a header to the app binary. Instead, a separate **flash table (ftab)** is stored at the beginning of flash (QSPI5 at `0x1C000000`). The bootloader reads it to locate and load the application.

**ftab.bin layout (32KB = 0x8000 bytes at `0x1C000000`):**

```
Offset    Size     Content
────────  ───────  ──────────────────────────────────────────────
0x0000    4B       Magic: 0x53454346 ('SECf')
0x0004    256B     flash_table[16] — partition entries (16 bytes each)
0x0104    294B     RSA-2048 public key
0x0230    3542B    Reserved
0x1000    7168B    image_header_enc[14] — per-image metadata (512 bytes each)
0x2C00    16B      running_imgs[4] — active image pointers per core
```

**Each partition entry (16 bytes):**
```c
struct flash_table {
    uint32_t base;      // storage/download address on flash
    uint32_t size;       // partition size
    uint32_t xip_base;   // execution/XIP address (for NOR flash, same as base + XIP offset)
    uint32_t flags;       // reserved
};
```

**Each image header (512 bytes):**
```c
struct image_header_enc {
    uint32_t    length;       // image size in bytes
    uint16_t    blksize;      // DFU block size
    uint16_t    flags;        // DFU_FLAG_ENC, DFU_FLAG_AUTO, etc.
    uint8_t     key[32];      // AES-256 encryption key
    uint8_t     sig[256];     // RSA-2048 signature
    uint8_t     ver[20];      // version string
    uint8_t     reserved[196];
};
```

**Partition indices (from `gen_ftab.py` `PartitionIndex` enum):**

| Index | Name | Description |
|-------|------|-------------|
| 0 | FLASH_PARTITION_TABLE | Flash table itself (ftab) |
| 1 | CALIBRATION_TABLE | Factory calibration data |
| 2 | LCPU_IMAGE_PING | LCPU image slot A |
| 3 | BOOTLOADER | Bootloader image |
| 4 | HCPU_IMAGE | Main HCPU application |
| 5 | BOOT_PATCH | Boot patch |
| 6 | LCPU_IMAGE_PONG | LCPU image slot B |
| 7 | BOOTLOADER_IMAGE_PONG | Bootloader slot B |
| 8 | HCPU_IMAGE_PONG | HCPU image slot B |
| 9 | RAM_BOOT_PATCH | RAM boot patch |
| 10-13 | Reserved | Extended image slots |
| 14-15 | Reserved | |

### 9.4 Bootloader Loading Sequence

The bootloader (`example/boot_loader/`) loads the application as follows:

```
Power on
  │
  ├─ board_flash_power_on()      → initialize flash controller
  ├─ HAL_MspInit()               → clocks, peripherals
  ├─ dfu_flash_init()            → init NOR flash (QSPI5), read sec_configuration
  │
  ├─ Validate magic (0x53454346)
  │
  ├─ boot_images_help()          → determine which image to boot
  │    └─ Read flash_table[HCPU_PARTITION] → get base, xip_base
  │
  ├─ dfu_boot_img_in_flash(flashid)
  │    ├─ If encrypted (DFU_FLAG_ENC):
  │    │    ├─ NOR flash: configure XIP AES (HAL_FLASH_AES_CFG) → execute from flash
  │    │    └─ NAND/RAM: copy to RAM → decrypt with sifli_hw_dec()
  │    ├─ If not encrypted:
  │    │    ├─ NOR flash: set up flash alias for XIP
  │    │    └─ NAND: copy from flash to execution address
  │    └─ Return destination address
  │
  └─ run_img(dest)               → jump to application
       ├─ LDR SP, [dest]         → load stack pointer from vector table[0]
       └─ LDR PC, [dest, #4]     → load reset handler from vector table[1]
```

**Key insight:** The app binary has **no special header**. It's a raw binary starting with the ARM Cortex-M vector table (initial SP at offset 0, Reset_Handler at offset 4). The bootloader uses the flash table (separate structure on QSPI5) to know where to find and how to load the app.

### 9.5 Partition Table Formats

The SDK supports three generations of partition table:

**ptab v1 (JSON array):**
```json
[
    {"version": "2"},
    {
        "mem": "flash2", "base": "0x14000000",
        "regions": [
            {"offset": "0x00020000", "max_size": "0x00700000",
             "tags": ["HCPU_FLASH_CODE"], "name": "main",
             "type": ["app_img", "app_exec"]}
        ]
    }
]
```

**ptab v3 (YAML):**
```yaml
partitions:
  - name: ftab
    type: ftab
    region: mpi5
    offset: 0x00000000
    size: 32KB
  - name: bootloader
    type: bootloader
    region: mpi5
    offset: 0x00020000
    size: 128KB
  - name: hcpu_app
    type: app
    subtype: factory
    region: mpi2
    offset: 0x00010000
    size: 960KB
    core: HCPU
    exec:
      region: mpi2
      offset: 0x00010000
```

### 9.6 Integrating Firmware Packaging into openvela

#### Approach A: Minimal — Pre-flashed ftab + bootloader (Recommended for contest)

The simplest approach: pre-flash ftab.bin and bootloader once, then only flash the app binary from openvela.

```
Flash Layout:
  QSPI5 (0x1C000000): ftab.bin (32KB) + bootloader (128KB)     ← pre-flashed once
  QSPI2 (0x12000000): [64KB reserved] + nuttx.bin (960KB)      ← openvela build output
```

**openvela build only needs:**
```cmake
# Post-build: generate raw binary
add_custom_command(TARGET nuttx POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:nuttx> nuttx.bin
  COMMENT "Generating firmware binary"
)
```

**Pre-flash steps (one-time, via JLink):**
```bash
# Flash ftab to QSPI5 base
JLinkExe -device SF32LB58 -if SWD -speed 4000 <<EOF
loadbin ftab.bin 0x1C000000
loadbin bootloader.bin 0x1C020000
r
g
exit
EOF
```

#### Approach B: Generate ftab.bin in openvela build

If you need ftab generation as part of the build:

```cmake
# Post-build: generate ftab.bin from partition table
add_custom_command(TARGET nuttx POST_BUILD
  COMMAND ${Python3_EXECUTABLE}
    ${SIFLI_SDK}/tools/build/gen_ftab.py
    --ptab ${BOARD_DIR}/ptab.yaml
    --output ${CMAKE_BINARY_DIR}/ftab.bin
  COMMENT "Generating flash table"
)

# Post-build: generate raw binary
add_custom_command(TARGET nuttx POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:nuttx> nuttx.bin
  COMMENT "Generating firmware binary"
)
```

**Required:** Create a `ptab.yaml` for your board with partition definitions.

#### Approach C: Combine into single flashable image

For a complete image that includes ftab + bootloader + app:

```cmake
# Combine all components into one image
add_custom_command(TARGET nuttx POST_BUILD
  COMMAND ${Python3_EXECUTABLE}
    ${SIFLI_SDK}/tools/build/combine_fw.py
    --ftab ${CMAKE_BINARY_DIR}/ftab.bin
    --bootloader ${BOOTLOADER_BIN}
    --app ${CMAKE_BINARY_DIR}/nuttx.bin
    --output ${CMAKE_BINARY_DIR}/full_image.bin
  COMMENT "Combining firmware image"
)
```

#### Approach D: Embed ftab in the linker output

The SiFli SDK's ptab v3 approach embeds ftab data directly in the ELF via a `.ftab` section:

1. Generate `ftab.c` from `ptab.yaml` via `gen_ftab.py --gen-src`
2. Compile `ftab.c` into the build (places data in `.ftab` section)
3. Add `.ftab` section to the linker script at the flash table address

```linker
.ftab : {
    *(.ftab)
} > flash_ftab    /* at 0x1C000000 */
```

This produces a single ELF/binary with everything, but requires a separate flash region in the linker script for QSPI5.

### 9.7 Recommended Approach

For the contest, **Approach A** is recommended:

1. **Pre-flash ftab + bootloader once** using JLink (these don't change during development)
2. **openvela build produces only the app binary** (nuttx.bin)
3. **Flash nuttx.bin to `0x12010000`** on QSPI2 (via JLink or the bootloader's DFU)
4. **No changes needed to the openvela build system** — it already produces a raw binary via `objcopy`

This matches how the SF32LB52 openvela build works — the app binary is just a raw ARM binary with vector table at the start, no special headers needed.
