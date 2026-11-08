# chiron 固件（`/lib/firmware`）

本分支放 **Xiaomi MIX 2（chiron / MSM8998）内核启动与工作时会用到的全部固件**，以及这份说明。
内核源码补丁在 `chiron-7.2.10` 分支上，二进制不进那里。

- 来源：本机（`7.2.10-xiaomi-chiron-server-r142-acd`）的 `/lib/firmware`，**原样取出**（含 `.zst` 压缩形式）
- 规模：**208 个文件 / 73,404,748 B（70.00 MiB）**，最大单文件 `modem.b12` 18,883,856 B
- 校验：`SHA256SUMS`（208 行，`sha256sum -c SHA256SUMS` 可验）

## 怎么用

```sh
# 1) 整个覆盖进固件搜索路径（保留目录结构）
sudo cp -r qcom qca ath10k regulatory.db regulatory.db.p7s /lib/firmware/

# 2) 校验
sha256sum -c SHA256SUMS

# 3) 生效：modem / adsp / GPU 的固件在对应 remoteproc / 驱动 probe 时加载，最省事是重启；
#    只换 WLAN 固件时：sudo modprobe -r ath10k_snoc && sudo modprobe ath10k_snoc
```

## 1. 调制解调器（MSS）—— 由 `&remoteproc_mss` 加载

DT 里写死（`arch/arm64/boot/dts/qcom/msm8998-xiaomi-chiron-wifi.dtsi:47`）：

```dts
&remoteproc_mss {
	firmware-name = "qcom/msm8998/xiaomi/chiron/mba.mbn",
			"qcom/msm8998/xiaomi/chiron/modem.mdt";
};
```

| 文件 | 大小 | 用途 |
|---|---|---|
| `qcom/msm8998/xiaomi/chiron/mba.mbn` | 224.7 KB | **MBA**（Modem Boot Authenticator）：PIL 先把它塞进 MSS，由它校验并跳转到 modem 镜像 |
| `qcom/msm8998/xiaomi/chiron/modem.mdt` | 8.2 KB | **MDT 元数据**：分片清单 + 每片 hash，`qcom_mdt_load()` 靠它决定加载哪些 `.bNN` |
| `qcom/msm8998/xiaomi/chiron/modem.b00` … `b28` | 合计 ≈ 56 MB | **分片镜像本体**（ELF 段按 4 KB 对齐切分）。本机有 `b00–b14`、`b16–b18`、`b20–b28`（没有 `b15`/`b19`，原厂如此）；最大 `b12` 18.0 MB、`b17` 15.0 MB、`b22` 10.5 MB |
| `qcom/msm8998/xiaomi/chiron/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig` | 32 B | **MCFG 硬件配置分区摘要**（modem 自检用，原厂只有一个占位摘要） |
| `qcom/msm8998/xiaomi/chiron/modem_pr/mcfg/configs/mcfg_sw/mbn_sw.dig` | 32 B | 同上，软件配置分区 |
| `qcom/msm8998/xiaomi/chiron/modemr.jsn` | 405 B | modem 配置索引（**r**etail）：告诉 modem 加载哪一组 MCFG |
| `qcom/msm8998/xiaomi/chiron/modemuw.jsn` | 713 B | 同上，**u**serdebug 变体 |

> 实测证据：`dmesg` 有 `Booting fw image qcom/msm8998/xiaomi/chiron/mba.mbn, size 230056`，
> 随后 `MBA booted without debug policy, loading mpss` —— 这条链是活的。

## 2. ADSP（音频 / 传感器 DSP）—— 由 `&remoteproc_adsp` 加载

DT 里写死（`msm8998-xiaomi-chiron-audio.dtsi:98`）：`firmware-name = "qcom/chiron/adsp.mdt";`

| 文件 | 大小 | 用途 |
|---|---|---|
| `qcom/chiron/adsp.mdt` | 7.1 KB | ADSP 的 MDT 元数据 |
| `qcom/chiron/adsp.b00/b01/b02/b03/b04/b05/b06/b08/b09/b11` | 合计 ≈ 11 MB | ADSP 分片镜像（没有 `b07`/`b10`，原厂如此）。`b05` 2.3 MB、`b08` 7.1 MB 是主体 |

> 注意路径是 `qcom/chiron/`，**不是** `qcom/msm8998/xiaomi/chiron/` —— DT 里就是这么写的。

## 3. GPU（Adreno 540）

| 文件 | 大小 | 谁请求 | 用途 |
|---|---|---|---|
| `qcom/msm8998/xiaomi/chiron/a540_zap.mdt` + `.b00/.b01/.b02` | 6.5 KB + 11 KB | `&adreno_gpu/zap-shader` 的 `firmware-name`（`chiron-gpu.dtsi:29`） | **Zap shader**：GPU 上电时由 TrustZone 执行，把 GPU 从安全世界接管出来；没有它 GPU 起不来 |
| `qcom/a540_gpmu.fw2` | 11.5 KB | `a5xx_catalog.c` 的 a540 条目（`ADRENO_FW_GPMU`） | **GPMU 固件**：GPU 的电源管理协处理器（频率/电压档、掉电时序） |
| `qcom/a530_pfp.fw` | 16.4 KB | 同上（`ADRENO_FW_PFP`） | **PFP 微码**：GPU 前端（primitive fetch/parse）。a540 复用 a530 的文件名 |
| `qcom/a530_pm4.fw` | 19.2 KB | 同上（`ADRENO_FW_PM4`） | **PM4 微码**：GPU 命令流解释器 |

> 加载顺序：`adreno_gpu.c` 先试 `qcom/<name>`（新位置），失败才退到 `<name>`（旧位置），
> 所以这些文件必须在 `qcom/` 下，不能平铺在 `/lib/firmware/` 根目录。

## 4. WLAN（QCA6174 / `ath10k_snoc`）

| 文件 | 大小 | 用途 |
|---|---|---|
| `qcom/msm8998/xiaomi/chiron/wlanmdsp.mbn` | 2.6 MB | WLAN MAC/DSP 固件（原厂命名） |
| `qcom/msm8998/xiaomi/chiron/bdwlan.bin` | 18.7 KB | **板级/校准数据**（board data），默认那一份 |
| `qcom/msm8998/xiaomi/chiron/bdwlan.{102,104,105,106,108,b04,b07,b09,b0a,b0b}` | 各 18.7 KB | 同一份板级数据的**变体**，按板型/版本索引（原厂整组都放进来了） |
| `ath10k/WCN3990/hw1.0/board.bin` | 18.7 KB | ath10k 的板级数据（`wcn3990 hw1.0` 这套路径） |
| `ath10k/WCN3990/hw1.0/firmware-5.bin` | **60 B** | 固件**版本占位**（不是真固件：WCN3990 的固件在芯片侧/NAND，由 QMI 上报版本）。原厂就这个大小 |

> 实测证据：`ath10k_snoc 18800000.wifi: wcn3990 hw1.0 target 0x00000008 … firmware ver api 5 … cal file max-sta 32`，
> 并且 `qmi chip_id 0x30214 … board_id 0xff`，说明这套路径就是本机的正常布局。
>
> **证据等级说明**：`qcom/…/wlanmdsp.mbn` 与 `bdwlan.*` 在内核源码里**没有按名字请求的位置**
> （`grep -rn 'wlanmdsp\|bdwlan' drivers/net/wireless` 0 命中；本机 `&wifi` 节点也没写 `firmware-name`）。
> 它们是原厂固件目录的配套内容，保留是为了一致性；删除前请先实机确认 WLAN 仍能起。

## 5. 蓝牙（`qca/`）—— 141 个文件

本机 BT 节点是 `qcom,wcn3990-bt`（挂在 `&blsp1_uart3` 上，`msm8998-xiaomi-chiron-bluetooth.dtsi`），
由 `btqca` 通过 `hci_ldisc` 走 UART 驱动，固件名在运行时按芯片的 ROM 版本拼出来：

```
qca/hpbtfw%02x.tlv   qca/htbtfw%02x.tlv   qca/cmbtfw%02x.tlv   qca/crbtfw%02x.tlv
qca/apbtfw%02x.tlv   qca/msbtfw%02x.mbn   qca/msbtfw%02x.tlv   qca/wcnhpbt%02x.tlv
qca/rampatch_%08x.bin + NVM（qca/*nv*.bin）
```

`qca/` 里就是这一大堆控制器（AP/CM/CR/MS/HP/HT/HMT/ORN/QCA6698…）的 **rampatch + NVM**，
全部是 `.zst` 压缩形式（内核的 `FW_LOADER_COMPRESS_ZSTD` 会自动去找 `<name>.zst`）。
因为文件名单在运行时才确定，整组收进来才不用猜。

> **重要**：本机**当前没有启用蓝牙** —— 没有 `/sys/class/bluetooth`，也没有加载 `btqca`/`hci_uart`，
> 所以今天没有任何东西会读这些文件。它们在这里是为了"哪天把 BT 拉起来时不用再去找"。

## 6. 监管数据库（`regulatory.db` / `regulatory.db.p7s`）

cfg80211 用它把国家码映射成频率/功率规则；没有它，WLAN 会退化到 world 域
（本机 `wlan0: regulatory prevented using AP config, downgraded` 就是没有合适规则的典型症状）。

- 来源：设备上的 `/lib/firmware`，内容由发行版的 `wireless-regdb` 包提供（**ISC 许可，可再分发**，
  和上面那些 Qualcomm/Xiaomi 二进制性质不同）
- 想用更新版本直接 `apt install wireless-regdb` 覆盖即可

## 统计

| 目录 | 文件数 | 说明 |
|---|---|---|
| `qcom/` | 63 | modem + ADSP + GPU + WLAN 板级数据 |
| `qca/` | 141 | 蓝牙 rampatch/NVM（`.zst`） |
| `ath10k/` | 2 | WCN3990 board data + 版本占位 |
| `regulatory.db*` | 2 | 监管数据库与签名 |
| **合计** | **208** | 73,404,748 B / 70.00 MiB |

## 来源与许可

- `qcom/`、`qca/`、`ath10k/` 全部取自本机 MIX 2 的 `/lib/firmware`（与小米原厂系统同一批二进制），未做任何修改。
- 它们是 **Qualcomm / Xiaomi 的专有二进制**，不是 GPL 代码。本仓库保存它们**仅为让这台设备的
  主线内核可以复现启动**，不声明任何再分发或商用授权；需要再分发请自行确认权利。
- `regulatory.db` / `regulatory.db.p7s` 例外：来自 `wireless-regdb`，ISC 许可。
- 所有文件与实机逐字节一致，用 `SHA256SUMS` 可核对。

## 校验

```sh
sha256sum -c SHA256SUMS                       # 208 个条目全部 OK 才算对
find qcom qca ath10k regulatory.db regulatory.db.p7s -type f | wc -l   # 208
find qcom qca ath10k -type f | wc -l          # 206（不含 regulatory 两个）
find qcom qca ath10k regulatory.db regulatory.db.p7s -type f -printf '%s\n' \
  | awk '{s+=$1} END {print s}'               # 73404748
```
