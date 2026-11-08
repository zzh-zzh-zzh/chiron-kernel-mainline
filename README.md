# chiron 固件（`/lib/firmware/qcom`）

本分支只放 **Xiaomi MIX 2（chiron / MSM8998）主线内核启动与工作时需要的专有固件**，
以及这份说明。内核源码补丁在 `chiron-7.2.10` 分支上，二进制不进那里。

- 来源：本机（`7.2.10-xiaomi-chiron-server-r142-acd`）的 `/lib/firmware/qcom`，**原样取出**
- 规模：**63 个文件 / 70,697,868 B（67.4 MiB）**，最大单文件 `modem.b12` 18,883,856 B
- 校验：`SHA256SUMS`（63 行，`sha256sum -c SHA256SUMS` 可验）

## 怎么用

```sh
# 1) 放进固件搜索路径（qcom/ 目录整个合并进去）
sudo cp -r qcom /lib/firmware/

# 2) 校验
sha256sum -c SHA256SUMS

# 3) 生效
#    - modem / adsp / GPU 的固件在对应 remoteproc / 驱动 probe 时加载，
#      最省事的办法是直接重启；
#    - 只换 WLAN 固件时：
sudo modprobe -r ath10k_snoc && sudo modprobe ath10k_snoc
```

## 这些固件是给谁的

### 1. 调制解调器（MSS）—— 由 `&remoteproc_mss` 加载

DT 里写死（`arch/arm64/boot/dts/qcom/msm8998-xiaomi-chiron-wifi.dtsi:47`）：

```dts
&remoteproc_mss {
	firmware-name = "qcom/msm8998/xiaomi/chiron/mba.mbn",
			"qcom/msm8998/xiaomi/chiron/modem.mdt";
};
```

| 文件 | 大小 | 用途 |
|---|---|---|
| `qcom/msm8998/xiaomi/chiron/mba.mbn` | 224.7 KB | **MBA**（Modem Boot Authenticator）：modem 镜像的引导认证器，PIL 先把它塞进 MSS，再由它校验并跳转到 modem 镜像 |
| `qcom/msm8998/xiaomi/chiron/modem.mdt` | 8.2 KB | **MDT 元数据**：分片清单 + 每片的 hash，`qcom_mdt_load()` 靠它决定加载哪些 `.bNN`、校验是否通过 |
| `qcom/msm8998/xiaomi/chiron/modem.b00` … `b28` | 合计 ≈ 56 MB | **分片镜像本体**（ELF 段按 4 KB 对齐切分）。本机存在的是 `b00–b14`、`b16–b18`、`b20–b28`（没有 `b15`/`b19`，原厂就是这样）；最大的三个是 `b12` 18.0 MB、`b17` 15.0 MB、`b22` 10.5 MB |
| `qcom/msm8998/xiaomi/chiron/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig` | 32 B | **MCFG 硬件配置分区的摘要**（modem 自检用；原厂就只有一个占位摘要） |
| `qcom/msm8998/xiaomi/chiron/modem_pr/mcfg/configs/mcfg_sw/mbn_sw.dig` | 32 B | 同上，软件配置分区 |
| `qcom/msm8998/xiaomi/chiron/modemr.jsn` | 405 B | modem 配置索引（**r**etail 变体）：告诉 modem 去加载哪一组 MCFG |
| `qcom/msm8998/xiaomi/chiron/modemuw.jsn` | 713 B | 同上，**u**serdebug 变体 |

> 实测证据：本机 `dmesg` 有 `Booting fw image qcom/msm8998/xiaomi/chiron/mba.mbn, size 230056`，
> 随后 `MBA booted without debug policy, loading mpss` —— 说明这条链是活的。

### 2. ADSP（音频 / 传感器 DSP）—— 由 `&remoteproc_adsp` 加载

DT 里写死（`msm8998-xiaomi-chiron-audio.dtsi:98`）：`firmware-name = "qcom/chiron/adsp.mdt";`

| 文件 | 大小 | 用途 |
|---|---|---|
| `qcom/chiron/adsp.mdt` | 7.1 KB | ADSP 的 MDT 元数据 |
| `qcom/chiron/adsp.b00/b01/b02/b03/b04/b05/b06/b08/b09/b11` | 合计 ≈ 11 MB | ADSP 分片镜像（同样没有 `b07`/`b10`，原厂如此）。`b05` 2.3 MB、`b08` 7.1 MB 是主体 |

> 注意路径是 `qcom/chiron/`，**不是** `qcom/msm8998/xiaomi/chiron/` —— DT 里就是这么写的，别改。

### 3. GPU（Adreno 540）

| 文件 | 大小 | 谁请求 | 用途 |
|---|---|---|---|
| `qcom/msm8998/xiaomi/chiron/a540_zap.mdt` + `.b00/.b01/.b02` | 6.5 KB + 11 KB | `&adreno_gpu/zap-shader` 的 `firmware-name`（`chiron-gpu.dtsi:29`） | **Zap shader**：GPU 上电时由 TrustZone 执行的签名片段，用来把 GPU 从"安全世界"接管过来；没有它 GPU 起不来 |
| `qcom/a540_gpmu.fw2` | 11.5 KB | `a5xx_catalog.c` 的 a540 条目（`ADRENO_FW_GPMU`） | **GPMU 固件**：GPU 的电源管理协处理器，负责频率/电压档与掉电时序 |
| `qcom/a530_pfp.fw` | 16.4 KB | `a5xx_catalog.c` 的 a540 条目（`ADRENO_FW_PFP`） | **PFP 微码**：GPU 前端（primitive fetch/parse）的固件。a540 复用的是 a530 的文件名 |
| `qcom/a530_pm4.fw` | 19.2 KB | 同上（`ADRENO_FW_PM4`） | **PM4 微码**：GPU 命令流解释器 |

> 加载顺序：`adreno_gpu.c` 先试 `qcom/<name>`（新位置），失败才退到 `<name>`（旧位置），
> 所以这些文件必须放在 `qcom/` 下，不能平铺在 `/lib/firmware/`。

### 4. WLAN（QCA6174，`ath10k_snoc`）

| 文件 | 大小 | 用途 |
|---|---|---|
| `qcom/msm8998/xiaomi/chiron/wlanmdsp.mbn` | 2.6 MB | WLAN MAC/DSP 固件（原厂命名） |
| `qcom/msm8998/xiaomi/chiron/bdwlan.bin` | 18.7 KB | **板级/校准数据**（board data），默认那一份 |
| `qcom/msm8998/xiaomi/chiron/bdwlan.102/.104/.105/.106/.108/.b04/.b07/.b09/.b0a/.b0b` | 各 18.7 KB | 同一份板级数据的**变体**，按板型/版本索引（原厂把整组都放进来了） |

> **证据等级要说明白**：本内核源码里 `grep -rn 'wlanmdsp\|bdwlan' drivers/net/wireless` **0 命中**，
> DT 的 `qcom,wcn3990-wifi` 节点也没写 `firmware-name`（ath10k 走 QMI 通道）。
> 也就是说这几个文件在内核侧**没有按名字请求的位置**，它们是原厂固件目录的配套内容，
> 保留在这里一是为了目录与原厂一致，二是 WLAN 子系统的自举链可能用到。
> **删除前请先在实机确认 WLAN 仍能起**（本仓库没有做过这个对照实验）。

## 不在本分支里的东西

| 文件 | 为什么不在 |
|---|---|
| `regulatory.db` / `regulatory.db.p7s` | 来自发行版的 `wireless-regdb` 包，不是本机专有固件；`apt install wireless-regdb` 即可 |
| `ath10k/WCN3990/hw1.0/board.bin`、`firmware-5.bin` | 本机不用这套路径（`firmware-5.bin` 只有 **60 字节**，是个占位而不是真固件） |

## 来源与许可

- 全部取自本机 MIX 2 的 `/lib/firmware/qcom`（即小米原厂系统里的同一批二进制），未做任何修改。
- 这些是 **Qualcomm / Xiaomi 的专有二进制**，不是 GPL 代码。本仓库保存它们**仅为让这台设备的
  主线内核可以复现启动**，不声明任何再分发或商用授权；需要再分发请自行确认权利。
- 文件内容与实机一致，用 `SHA256SUMS` 可以逐字节核对。

## 校验

```sh
sha256sum -c SHA256SUMS                                   # 63 个文件全部 OK 才算对
find qcom -type f | wc -l                                 # 63
find qcom -type f -printf '%s\n' | awk '{s+=$1} END {print s}'   # 70697868
```
