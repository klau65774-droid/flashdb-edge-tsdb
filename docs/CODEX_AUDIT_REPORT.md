# FlashDB Edge TSDB 源码审计与回归报告

审计日期为 2026-11-24。范围覆盖 C11 内核、CLI、STM32 FAL/UART 移植、树莓派文件采集和相关测试。审计先检查现有路径和接口，再对确认的问题修改实现并补充可单独运行的 CTest。FlashDB 的 `src` 和 `inc` 基线共 10 个文件，审计前后的文件 SHA-256 汇总均为 `BAF262DA9A32CB36C9883F5C1D79616DEDA787C54FDD0A40908D64E889067269`。

## 确认问题与修复

严重度依据数据持久性、Flash 回写、接收端 ACK 和资源边界的后果评定。行号以修复后的文件为准。

| 级别 | 位置 | 根因与影响 | 修复 | CTest |
| --- | --- | --- | --- | --- |
| Critical | — | 当前审计范围内未发现此级别的确认问题。 | — | — |
| High | `src/fdb_edge_tsdb.c:298`、`:309`、`:895`、`:943` | 分裂刷盘的前缀成功后，后缀 I/O 或 FULL 失败会保留整个暂存区。后续查询和刷盘可重复前缀，自动刷盘失败后继续追加还可越过暂存容量。 | 分片以已提交边界为准，从暂存区移除已提交项。提交尾状态读不清时先重新扫描，再决定消耗多少前缀。追加调用返回的非零时间戳序号标识已接收的样本。 | `split_flush_retry`、`split_flush_full` |
| High | `src/fdb_edge_tsdb.c:474`、`:499`、`:542`、`:574`、`:606`、`:723` | 失败的封口、擦除、激活和尾块程序操作可能让活句柄继续相信旧状态。受损物理页可被重写，擦除后若激活头未完成，旧磨损计数也会丢失。 | 对失效页隔离或读回复核，验证空白扇区全区，优先回收无有效数据的扇区，回卷时按最旧代次选择有效扇区。封口预留区写入独立 CRC 保护的擦除意图。 | `nor_rollover_recovery`、`split_flush_retry` |
| High | `src/fdb_edge_tsdb.c:239`、`:291`、`:994`、`:1000` | 重开或原始块导入缺少完整 HLC 状态。重叠块或先提交原始块后刷本地暂存会破坏序号顺序和时间状态。 | 恢复最后样本的墙钟值、序号、epoch 和逻辑子计数。先校验导入范围和时间序，再提交本地暂存并导入原始块。 | `raw_hlc_recovery` |
| High | `src/fdb_edge_wal_sync.c:504`、`src/fdb_edge_tsdb.c:1095` | 接收端信任帧头序号范围和部分重叠，ACK 可能先于真实已落盘序号前进。 | 在写入前比对已解码端点，拒绝新旧范围部分重叠。按提交范围确认，终止序号下不回绕下一序号。 | `wal_sequences`、`rpi_collector_file_roundtrip` |
| High | `ports/raspberry_pi/rpi_edge_collector.c:53`、`:102`、`:227`、`:297` | 参数截断、镜像尺寸和文件别名处理不足。原采集流程也忽略持久化和部分 ACK I/O 结果。 | 校验几何、尺寸、打开方式与文件身份。收齐一帧后处理回复，检查读写和关闭错误，在回复前将已提交数据同步到文件。 | `rpi_collector_file_roundtrip` |
| Medium | `include/fdb_edge/fdb_edge_config.h:34`、`src/fdb_edge_tsdb.c:114`、`:895`、`ports/stm32/fdb_cfg.h:25` | 扇区宏被固定为 1,024，查询与重开使用按扇区上限分配的栈数组，端口 SRAM 说明低估实际占用。 | 允许统一覆盖扇区上限。使用无大栈数组的代次迭代，分裂刷盘使用有界小栈。STM32 配置选用 128 扇区并交叉编译核算。 | `stm32_profile_and_fal_io`、`nor_rollover_recovery` |
| Medium | `ports/stm32/stm32_fal_flash_port.c:16`、`:51`、`:108` | FAL 短传输被存储层当作成功。UART 容量不足以容纳全量 WAL 时，已装好的完整帧也没有发送。 | 端口核对读写及擦除长度和分区对齐。发送可容纳的完整前缀并按 ACK 续传。 | `stm32_profile_and_fal_io`、`wal_export` |
| Medium | `src/fdb_edge_tsdb.c:14`、`:139`、`:351`、`src/fdb_edge_flash_sim.c:17`、`:135` | 几何和长度运算可回绕。最小提交尾为 8 字节，原数组可能被配置得更小。 | 使用除法与剩余容量检查，约束编程粒度、解析长度和填充后大小。提交尾局部缓冲至少 8 字节。 | `flash_geometry_bounds`、`test_flash_sim_bounds` |
| Medium | `src/fdb_edge_codec.c:49`、`:84`、`:141`、`:202`、`:479`、`:490` | 旧 ZigZag 和时间戳恢复依赖实现定义的整数转换，序号加法和 epoch 边界未完整验证。部分非规范 varint 和尾部填充被接受。 | 采用无符号模运算和可表示的有符号恢复，校验序号、epoch、varint、XOR 窗口与尾部。按字节块处理 Gorilla 比特，保留 C1 编码。 | `test_codec_timestamp_and_ieee_bits`、`test_codec_malformed_input`、`test_codec_wire_compatibility` |
| Medium | `src/fdb_edge_wal_sync.c:59`、`:167`、`:319`、`:504` | COBS 在终止的 254 字节非零段和零输出容量上判断错误。接收与解码叠加大栈缓冲。 | 先计算实际编码长度，使用有界缓冲，保留旧帧解码，验证超长帧后按分隔符恢复。 | `wal_cobs`、`wal_resync` |
| Medium | `src/fdb_edge_privacy.c:42`、`:72`、`:107`、`:245` | PEM 和 DSN 在长输入上重复回扫，PEM 任意标签的结束行可跨越相邻内容，引号内带空格的凭证只处理首段。 | PEM 标签定长上限、结束标签精确匹配且候选扫描单调推进。DSN 每个连续 scheme 只扫一次，完整引号内凭证统一替换。 | `privacy_pem_boundaries`、`privacy_adjacent_credentials`、`privacy_max_size` |
| Medium | `cli/fdb_edge_cli.c:346`、`:406`、`:511`、`:780`、`:938`、`:1003` | CSV 几何、数值和 I/O 错误可使基准覆盖失真。WAL 原判断只比较计数，故障试验未完整验证断电后持久化写入。 | 严查 CSV 和几何并检查数据库调用，按序核对时间戳与逐通道位值，故障试验用重开回读检查已提交样本和恢复后写入。 | `test_cli_audit` |
| Low | `tests/test_wal_sync_and_privacy.c:55` | 原 WAL 测试使用 128 KiB 单栈数组。 | 将测试缓冲移至静态存储。 | `test_wal_sync_and_privacy` |

## 数据持久性语义

- 同一句柄在刷盘错误后可重试未提交后缀。冷重开恢复至有效提交尾。真正掉电时未写入 Flash 的 RAM 样本由调用方按自己的采集持久化约定处理。
- 在保留有效前驱擦除意图的条件下，测试对照模拟器计数验证回卷后撕裂激活头的恢复。若受害扇区激活头仍有效，使用它的计数。擦除中断、意图撕裂或多个连续故障耗尽元数据时，物理擦除量可能无法从现存字节精确推回。
- C1 与扇区主体布局沿用原格式。C1 黄金向量及 1,001 个新旧版本编码对照块逐字节一致。解码回归覆盖 `INT64_MIN`、`INT64_MAX`、IEEE-754 NaN 位载荷、无穷值、负零、次正规数及截断。

## 构建和 CTest 实测

| 工具链 | Edge 目标的编译选项 | Edge 警告 / 错误 | CTest |
| --- | --- | ---: | ---: |
| MSVC x64 19.50.35725，NMake Release | `-std:c11 /W4 /WX` | 0 / 0 | 23/23 |
| MinGW-W64 GCC 16.2.0，Release | `-std=c11 -Wall -Wextra -Wshadow -Werror` | 0 / 0 | 23/23 |
| Arm Compiler for Embedded 6.19，Cortex-M4 和 M7 | `-std=c11 -O2 -Wall -Wextra -Wshadow -Werror` | 0 / 0 | 每个目标编译 7 个内核及端口源文件，另编译 `MAX_WRITE_GRAN=1` 的 TSDB 配置 |

MSVC 干净构建中的 11 条 `C4311` 与 GCC 干净构建中的 14 条告警均来自 `upstream/FlashDB/src/fdb_tsdb.c`。MSVC 输出 `100% tests passed, 0 tests failed out of 23`。GCC 输出 `100% tests passed out of 23`。两次最终 CTest 均耗时 1.19 秒。Release 下回归用例的 `CHECK()` 保持生效。五项 TSDB 回归以隔离的旧行为变体做过负向验证，均触发预期断言。畸形序列测试链接旧编码器对象时也按预期失败。

在 MSVC x64 开发命令环境及工作区根目录中复跑

```bat
call "%VS_BUILDTOOLS%\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=%VS_BUILDTOOLS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure -V
```

干净构建另外使用了 `cmake --build build --clean-first`。GCC 命令在 MinGW 的 `bin` 已加入 PATH 后运行

```powershell
cmake -S . -B build/gcc-audit -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
cmake --build build/gcc-audit -j 6
ctest --test-dir build/gcc-audit --output-on-failure -V
```

本地原始日志在 `build/msvc-audit-build.log`、`build/msvc-audit-final-build.log`、`build/msvc-audit-ctest.log`、`build/gcc-audit-build.log`、`build/gcc-audit-final-build.log`、`build/gcc-audit-ctest.log`。STM32 FAL 在主机上运行合同测试，对 1、4、8、32 字节粒度验证短 I/O、边界与重开。树莓派采集回归在 Windows 生成 WAL，验证 80 个样本冷重开读取、重放幂等，以及首轮和重放各 170 个 ACK/NACK 的类型和序号。

## 三层数据复跑

使用 Python 3.14.3 和指定的数据目录运行原脚本。`FDB_EDGE_BENCHMARK_ROOT` 指向所提供的外部数据集目录。

```powershell
python benchmarks/run_benchmarks.py --dataset-root "$env:FDB_EDGE_BENCHMARK_ROOT"
```

退出码为 0，标准输出如下。各文件明细、Flash 字节、擦除和耗时由脚本写入 [benchmarks_report.json](benchmarks_report.json)。

```json
{
  "status": "verified",
  "tier1_verdict": "PASSED",
  "tier1_rows": 69561,
  "tier1_up_drop": 23,
  "tier1_write_reduction_x": 3.0472,
  "tier2_verdict": "PASSED",
  "tier2_imu_rows": 13514,
  "tier2_imu_write_reduction_x": 1.643,
  "tier2_faults_passed": "1000/1000",
  "tier2_sync_exact": true,
  "tier3_verdict": "PASSED"
}
```

| 场景 | 实测结果 |
| --- | --- |
| 上游回拨和容量复现 | 回拨时上游 10 成功、5 丢弃，Edge 15/15 查回。容量下溢复现值 4,294,967,262，Edge 返回 `FDB_EDGE_ERR_CAPACITY`，16 KiB 扇区配置查回 64/64。 |
| Tier 1 · NAB 七份真实时间序列 | 69,561/69,561 逐位一致，0 NOR 违规。上游 1,839,123 B → Edge 603,548 B，缩减 3.0472×。注入 1 小时回拨后查回 22,695/22,695，缩减 2.7174×。 |
| Tier 2 · Fusion 9 轴 IMU | 13,514/13,514 逐位一致，`max_abs_err=0`、0 NOR 违规。上游 1,226,533 B → Edge 746,509 B，缩减 1.643×。 |
| Tier 2 · 断电仿真 | 1,000/1,000 实际触发的断电试验通过，1,000 个撕裂尾部被隔离，已提交数据丢失和 NOR 违规均为 0。1、4、8、32 字节写入粒度均覆盖整字节和部分字节断电模式。 |
| Tier 2 · WAL | 源端和目标端各 2,000 条逐位一致。注入 2 次损坏及 1 次重放，拒绝 14 次乱序帧，2 轮完成。 |
| Tier 3 · 内置合成隐私与边界夹具 | 召回 15/15，良性误报 0/15，边界检查 4/4。 |

## STM32 SRAM 实测

使用 `tests/stm32_layout_probe.c` 读取 Cortex-M4/M7 目标的汇编符号大小，使用 `-fstack-usage` 获取函数栈帧。此处的 STM32 默认配置为 128 扇区、8 通道、64 个样本和 4,096 字节工作区。以相同设置对比旧 1,024 扇区上限。

| 对象或函数栈帧 | Cortex-M4 | Cortex-M7 |
| --- | ---: | ---: |
| `sizeof(fdb_edge_tsdb_t)` · 128 扇区 | 25,736 B | 25,736 B |
| `sizeof(fdb_edge_tsdb_t)` · 1,024 扇区 | 90,248 B | 90,248 B |
| 每个扇区元数据 / 每个 8 通道样本 | 72 B / 96 B | 72 B / 96 B |
| `sizeof(fdb_wal_receiver_t)` | 4,152 B | 4,152 B |
| `fdb_edge_tsdb_open` / `scan_sector_blocks` | 200 B / 240 B | 200 B / 240 B |
| `fdb_edge_tsdb_flush` / `commit_encoded_block` | 152 B / 304 B | 152 B / 296 B |
| WAL 编码 / 公开解码 / 接收 | 96 B / 192 B / 160 B | 104 B / 192 B / 168 B |
| 公开解码的右移重叠缓冲处理 | 592 B | 584 B |

仅扇区上限配置减少 64,512 B。默认 TSDB 加一个接收器共 29,888 B，UART TX 缓冲由调用方提供。栈表记录单函数帧，实际栈预算需累加活跃调用链和固件上下文。交叉编译的汇编和 `.su` 文件位于 `build/arm-audit/`。[端口配置与复现参数](../ports/stm32/stm32_toolchain_notes.md)

## 验收结论

**PASS WITH WARNINGS**。源码、主机构建和回归、三层数据校验及 Cortex-M 交叉编译通过。STM32 实机断电和 Raspberry Pi 实机运行未测，采集器的文件回归运行在 Windows。物理擦除计数以可恢复的头部与擦除意图为证据，任意连续掉电下的精确次数需要独立持久计量。上游基线保留其原始诊断。
