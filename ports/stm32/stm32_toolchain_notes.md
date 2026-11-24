# STM32 Integration and SRAM Budget

Force-include `ports/stm32/fdb_cfg.h` in every library and application
translation unit that uses an Edge TSDB type. The limit macros determine
public structure layouts. Set overrides consistently across the target.

## Cortex-M build flags

`arm-none-eabi-gcc` settings for a Cortex-M4 firmware target with FAL headers
on the include path

```sh
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
  -std=c11 -O2 -Wall -Wextra -Wshadow -Werror -ffunction-sections -fdata-sections \
  -include ports/stm32/fdb_cfg.h -DFDB_EDGE_STM32_HAL -Iinclude \
  -I"$FAL_INCLUDE" -c \
  src/fdb_edge_crc32c.c src/fdb_edge_hlc.c src/fdb_edge_codec.c \
  src/fdb_edge_tsdb.c src/fdb_edge_wal_sync.c src/fdb_edge_privacy.c \
  ports/stm32/stm32_fal_flash_port.c
```

Select the CPU and FPU flags matching the firmware toolchain and the other
objects linked into the image. Allocate the TSDB and any WAL receiver in
static storage. The UART transmit buffer is caller-owned, and a successful
send may represent only the complete frames that fit in that buffer.

## Measured structure sizes

Sizes come from symbols in the assembly emitted for `tests/stm32_layout_probe.c`
with Arm Compiler for Embedded 6.19 targeting Cortex-M4 and Cortex-M7.
The default STM32 profile uses 128 sectors, 64 staged samples, 8 channels
and 4,096 workspace bytes. Both ARM targets reported the same sizes.

| Object or configuration | Cortex-M4 | Cortex-M7 |
| --- | ---: | ---: |
| `sizeof(fdb_edge_tsdb_t)`, default STM32 profile | 25,736 B | 25,736 B |
| `sizeof(fdb_edge_tsdb_t)`, STM32 settings with 1,024 sectors | 90,248 B | 90,248 B |
| `sizeof(fdb_edge_tsdb_t)`, all host defaults on ARM ABI | 127,112 B | 127,112 B |
| `sizeof(fdb_edge_sec_meta_t)` | 72 B | 72 B |
| `sizeof(fdb_edge_sample_t)`, 8 channels | 96 B | 96 B |
| `sizeof(fdb_wal_receiver_t)` | 4,152 B | 4,152 B |

The 128-sector profile supports a 512 KiB partition with 4 KiB sectors.
Increase the limit if the partition needs more sectors. Each added sector
uses 72 bytes of SRAM with the measured ABI. The flash and WAL headers use
explicit little-endian serialization with byte accesses.

## Measured stack frames

Arm Compiler for Embedded 6.19 emitted these per-function values with
`-O2 -fstack-usage` and the default STM32 profile. A live call chain
includes the frames of the caller and its callees.

| Function | Cortex-M4 | Cortex-M7 |
| --- | ---: | ---: |
| `fdb_edge_tsdb_open` | 200 B | 200 B |
| `scan_sector_blocks` | 240 B | 240 B |
| `fdb_edge_tsdb_flush` | 152 B | 152 B |
| `commit_encoded_block` | 304 B | 296 B |
| `query_internal` | 160 B | 160 B |
| `fdb_wal_encode_frame` | 96 B | 104 B |
| `fdb_wal_decode_frame` | 192 B | 192 B |
| `copy_shifted_payload` | 592 B | 584 B |
| `fdb_wal_receiver_feed` | 160 B | 168 B |
| `fdb_stm32_flush_wal_over_uart` | 48 B | 40 B |

The decoder calls `copy_shifted_payload` only for a right-shifted,
overlapping payload destination. The UART receiver decodes in place in its
own static buffer. The default TSDB and receiver total 29,888 bytes of RAM
before the application's TX buffer and stack.

## Flash integration

| MCU / flash backend | `write_gran` in bytes | Verification |
| --- | ---: | --- |
| STM32F4 or external SPI NOR | 1 or 4 | Host FAL contract test, Cortex-M4 build |
| STM32L4 / G4 internal flash | 8 | Host FAL contract test |
| STM32H7 internal flash | 32 | Host FAL contract test, Cortex-M7 build |

The host FAL suite checks partition alignment, short transfers and
write/read/reopen at all four program widths. The cross-build uses the
repository's FAL declarations. Board-specific FAL, RTOS, IRQ and DMA
memory costs depend on the firmware and require measurement on the target.
See [audit results](../../docs/CODEX_AUDIT_REPORT.md) for the regression suite.
