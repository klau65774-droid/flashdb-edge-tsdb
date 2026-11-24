# FlashDB Edge TSDB — Technical Design & Architecture

## 1. Design Objectives (STM32 Cortex-M + Raspberry Pi Gateway)

`flashdb-edge-tsdb` upgrades unmodified [`armink/FlashDB`](https://github.com/armink/FlashDB) (commit `6c722f169f27f99f96670ee00c27b2f59b1505e2`, `Apache-2.0`) in pure C11 without breaking the original library (`upstream/FlashDB/` remains untouched for baseline comparison).

Key engineering additions target dual deployment on resource-constrained **STM32 MCUs** (`write_gran` = `1 / 4 / 8 / 32` bytes, zero heap allocation in core paths) and **Raspberry Pi ARM64 Linux gateways**:

1. **Monotonic Hybrid Logical Clock (`fdb_edge_hlc`)**: Fixes Upstream Issue `#16` (`fdb_tsdb.c:472`), where NTP/RTC backward steps or same-tick burst writes are dropped with `FDB_WRITE_ERR`.
2. **Capacity Underflow Guard (`fdb_edge_tsdb_open`)**: Fixes Upstream Issue `#249` / unsigned `sector->remain` wraparound (`fdb_tsdb.c:495`, `1043`), enforcing explicit sector-usable-space checks (`sector_size >= sector_hdr + block_hdr + align_up(max_block_bytes, write_gran) + commit_trailer`) and adaptive split-flushing on high-entropy payloads.
3. **Strict NOR Flash `1 -> 0` Layout & Crash Consistency (`fdb_edge_tsdb`)**: Enforces bit-AND physical NOR rules (`0` bit violations even at `write_gran = 32`), split sector headers (`32 B` Active + `64 B` Seal), `CRC32C` headers/payloads, and commit trailers written strictly last.
4. **Zero-Malloc Time-Series Compression (`fdb_edge_codec`)**: Combines ZigZag + Delta-of-Delta timestamps with bit-exact 64-bit IEEE-754 Gorilla XOR channel streams (`1.64x–6.24x` Flash write reduction with `max_abs_err = 0`).
5. **STM32 <-> Raspberry Pi Incremental WAL Sync (`fdb_edge_wal_sync`)**: COBS-framed (`0x00`-delimited), `CRC32C`-verified compressed block transport with `ACK`/`NACK` flow control and idempotent sequence deduplication.
6. **Zero-Heap Edge Privacy Scrubber (`fdb_edge_privacy`)**: Redacts `IMSI`, `IMEI`, `MSISDN`, Bearer tokens, API keys, PEM private keys, Wi-Fi PSKs, and DSN passwords before gateway forwarding.

---

## 2. On-Flash Layout (Strict NOR `1 -> 0` Compliance)

Every flash sector (`sector_size >= 256`, power of 2) is laid out as follows:

| Offset Range | Size | Region | Written When | Protected By |
| :--- | :--- | :--- | :--- | :--- |
| `0 .. 31` | `32 B` | **Active Sector Header** (`magic=0x46444253`, `version=1`, `sector_seq`, `erase_count`) | Once on sector activation | `active_crc32c` |
| `32 .. 79` | `48 B` | **Seal Summary** (`min/max_mono_seq`, `min/max_wall_ms`, `sample_count`, `block_count`, `sealed_magic=0x5345414C`) | Once on sector rotation | `seal_crc32c` |
| `80 .. 95` | `16 B` | **Optional erase intent** (marker, victim index, next wear count) | In the same program operation as the seal before a rollover erase | Independent CRC32C |
| `96 .. sector_size-1` | Variable | **Committed Compressed Blocks** (append-only) | Sequential block commits | Per-block `hdr_crc32c` + `payload_crc32c` + `Commit Trailer` |

Each committed block at offset `pos` has 3 sequential write phases:

1. **Block Header (`64 B`)**: `magic=0x46444242`, `payload_len`, `sample_count`, `num_channels`, `min/max_mono_seq`, `min/max_wall_ms`, `min/max_epoch`, `payload_crc32c`, `padded_payload_len`, `hdr_crc32c`.
2. **Compressed Payload (`align_up(payload_len, write_gran)` bytes)**: `C1` header + varint timestamps/seq/epochs + base `double` values + Gorilla XOR bitstream.
3. **Commit Trailer (`align_up(8, write_gran)` bytes — written strictly LAST)**: `commit_magic=0x434D5452` + `commit_crc32c = payload_crc32c ^ 0xA5A55A5A`.

On reboot, `fdb_edge_tsdb_open` scans committed blocks through the last valid header, payload CRC and commit trailer. Live I/O failure fences the affected tail or sector before further programs. A valid erase intent can restore wear metadata when the victim loses its activation header. The intent shares the original seal program, so 32-byte program units are written once and existing sector offsets and version remain valid. Flash timing can make an interrupted physical erase ambiguous. See the audit report for the measured recovery cases and counting limits.

A split flush consumes each committed prefix even when a later slice fails. Its RAM buffer retains the remaining suffix for a subsequent flush on the same handle. Persistence across a reboot extends through the last committed trailer. A nonzero `out_stamp->mono_seq` indicates that an append reached staging even when its automatic flush returns an error.

---

## 3. HLC Epoch Rollback Algorithm (Issue #16)

`fdb_edge_hlc_next(&hlc, wall_ms, &out)`:
- Every sample monotonically increments `monotonic_seq` (`>= 1`).
- If `wall_ms < last_wall_ms` (e.g., NTP/RTC `-1h` step): increments `epoch` and `rollback_count`, sets `last_wall_ms = wall_ms` so subsequent samples in the corrected timeline continue cleanly without false rollback storms, and marks `was_rollback = true`.
- If `wall_ms == last_wall_ms`: increments `logical_sub` so burst writes at the same millisecond remain strictly ordered and never rejected.

---

## 4. COBS + CRC32C WAL Sync Frame Wire Format

Raw frame bytes before COBS encoding:

| Byte Offset | Size | Field |
| :--- | :--- | :--- |
| `0..1` | `2 B` | Magic `'W'`, `'L'` |
| `2` | `1 B` | Wire version (`1`) |
| `3` | `1 B` | Message type (`1=DATA`, `2=ACK`, `3=NACK`, `4=HEARTBEAT`) |
| `4..7` | `4 B` | `device_id` (LE) |
| `8..15` | `8 B` | `from_seq` (LE) |
| `16..23` | `8 B` | `to_seq` (LE) |
| `24..25` | `2 B` | `payload_len` (LE) |
| `26..27` | `2 B` | `flags` (LE) |
| `28..28+payload_len-1` | `N B` | Compressed TSDB `C1` block bytes (reused directly without recompression) |
| Last `4 B` | `4 B` | Castagnoli `CRC32C` over bytes `0 .. 28+payload_len-1` |

The raw frame is COBS-encoded and terminated with `0x00` so UART DMA streams on STM32 can sync packet boundaries deterministically after arbitrary byte splits or line noise.

The receiver verifies the sequence range against the decoded DATA block before it acknowledges a new commit. A bounded export can return a prefix of complete frames on `FDB_EDGE_ERR_BOUNDS`. The STM32 bridge sends that prefix and uses the peer's ACK cursor to resume. Configure the source block budget to fit the encoded frame limit, including WAL metadata and framing overhead.

---

## 5. Build Profiles: STM32 vs Raspberry Pi

- **STM32 (`ports/stm32/`)** uses 8 channels, 64 samples per batch, 4,096 workspace bytes and 128 tracked sectors by default. Force-include the port configuration in the library and its consumers. The host default remains 1,024 tracked sectors. The FAL bridge rejects short transfers and checks partition alignment and capacity. [STM32 layout and stack measurements](../ports/stm32/stm32_toolchain_notes.md)
- **Raspberry Pi (`ports/raspberry_pi/`)** receives WAL into a file-backed NOR image. The collector validates geometry and existing image size before ingest, checks file transfers, and synchronizes image updates before emitting ACKs.

## 6. Privacy Scan

The scrubber bounds input size and caps private-key labels at a 64-byte header. A footer must carry the exact opening label. If a footer is missing, scanning resumes at the next recognized header and other credential rules still apply to the unmatched text. Quoted credentials support spaces and escaped delimiters. The DSN parser evaluates each candidate scheme run once.
