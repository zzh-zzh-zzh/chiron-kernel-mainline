# Chiron (Xiaomi Mi MIX 2) kernel

This tree is mainline **v7.2.10** (stable) plus the patches needed to run on the
Xiaomi Mi MIX 2 (chiron / MSM8998).  The patches are the 12 commits on top of
the `v7.2.10` tag; nothing else is modified.

- Branch `chiron-7.2.10` — source only
- Branch `firmware` — the proprietary blobs the built kernel needs at runtime
  (`/lib/firmware`), with a README describing what each one is for

## Build

Everything below was last verified with the toolchain listed, on a Linux host
(WSL2 Ubuntu works; the build has to run on a case-sensitive filesystem).

### Toolchain

| tool | version used |
| --- | --- |
| clang / ld.lld | 21.1.8 (Ubuntu 21.1.8) |
| GNU make | 4.4.1 |
| pahole | 1.31 |
| python3 | 3.14 |
| xz / zstd | 5.8.3 / 1.5.7 |
| bc / dtc | 1.07.1 / 1.7.2 |

Debian/Ubuntu package set:

```sh
sudo apt-get install -y --no-install-recommends \
  clang lld llvm libssl-dev bc bison flex libelf-dev cpio \
  xz-utils zstd device-tree-compiler python3 pahole
```

LLVM=1 implies ThinLTO, which is what the released images were built with.

### Commands

The defconfig in this tree is the exact configuration the released images use:

```sh
# 1) 配置（会写到 O= 指定的输出目录，不污染源码树）
make O=$PWD/../build-chiron ARCH=arm64 LLVM=1 chiron_defconfig

# 2) 编译
make O=$PWD/../build-chiron ARCH=arm64 LLVM=1 -j"$(nproc)" Image.gz dtbs modules
```

Products:

```
build-chiron/arch/arm64/boot/Image.gz
build-chiron/arch/arm64/boot/dts/qcom/msm8998-xiaomi-chiron.dtb
build-chiron/**/*.ko                                  (166 modules)
build-chiron/include/config/kernel.release
```

`kernel.release` reads `7.2.10-xiaomi-chiron-server-r142-acd`; change
`CONFIG_LOCALVERSION` in the defconfig if you want a different suffix.

Reference timings on a 16-thread / 7 GB VM with `-j6`: about 17–22 minutes for
a full `Image.gz dtbs modules` build, most of it in the ThinLTO link.

### Boot image

The boot partition takes an Android header v0 image whose payload is
`Image.gz` immediately followed by the DTB, plus a ramdisk:

```sh
cat arch/arm64/boot/Image.gz arch/arm64/boot/dts/qcom/msm8998-xiaomi-chiron.dtb > /tmp/payload
mkbootimg --header_version 0 --base 0 --pagesize 4096 \
  --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 \
  --second_offset 0x00f00000 --tags_offset 0x00000100 \
  --cmdline 'console=tty0 console=ttyMSM0,115200n8 earlycon=msm_serial_dm,0xc1b0000 clk_ignore_unused regulator_ignore_unused panic=10 root=UUID=<rootfs-uuid> rootfstype=ext4 rw rootwait fsck.repair=yes' \
  --kernel /tmp/payload --ramdisk <initrd> --output boot.img
```

The device boots it from `/dev/sde40` (`by-partlabel/boot`, 64 MiB); the
stock bootloader (ABL) is used unchanged, and module loading needs the matching
`/lib/modules/<kernel.release>/` on the root filesystem.

### Firmware

The kernel itself builds without firmware, but it will not bring up the modem,
ADSP, GPU or WLAN without the blobs — they live on the `firmware` branch of
this repository, together with a README that says what each file is and which
driver or DT node asks for it.
