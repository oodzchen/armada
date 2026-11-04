# 一加平板 2 Pro（SM8750）Android / Armada 双启动方案

> 状态（2026-11-04）：完整 Armada 已通过临时启动从 USB 进入桌面和游戏模式。显示黑屏修复 1406/1407 已提交（`98f8101`），临时启动 16/16 通过；桌面网页音频、音量和静音已由用户确认正常，六路350增益也已试听通过（§19.38）。音频内核 1408、拓扑、UCM、固件输入检查及 initramfs 收录已接入源码构建（§19.31）；平板无内置手柄导致的服务误报已修复，系统恢复 `running`、失败单元为 0（§19.32）。armada19 独立镜像（`1768f477…`，自身包含权限与350增益）已通过全新可写层首启/再启及原配置启动验收（§19.39）。kShimC 已写入 `boot_b`（`9d91e3a9…`，保留 AVB vbmeta），开机先出菜单、默认 ColorOS，Armada 仍从 U 盘运行，不再需要电脑 fastboot。
> 本文中标注 **【待验证】** 的内容尚未在实机上确认，不能当作事实使用。
> **继续工作时先读第 21 节（最新验收、测试清单与机内双启动缺口），实现证据见 19.30–19.38，显示问题再读 19.23–19.28。** 第 17、18、20 节及更早测试清单包含历史状态；其中旧“下一步”、补丁计数和未实现状态不代表当前进度。不要从已完成的 UFS NOP、EFI、erhai 点屏或 Armada 触控测试重新开始。

## 当前进度与剩余工作

| 阶段 | 当前状态 | 仍需完成 |
|---|---|---|
| 一：采集与备份 | 检查点已通过 | 后续每轮继续核对原镜像与当前槽位 |
| 二：kShimC 与 Android | 检查点已通过；kShimC 已写入 `boot_b`，从闪存启动的菜单/ColorOS 正常（§19.39） | 长期回归；每次换 Armada 启动文件需重写 `boot_b` |
| 三：U-Boot 可行性 | 基本通过：显示、内存、返回 Android、UFS 唤醒/只读/恢复、6 个 LUN 主备 GPT（66/66）、只读块设备上的 FAT16/ext4（4/4 CRC 一致）、EFI loader（ABL 交接内存保留并 CRC 校验）、systemd-boot 菜单（音量/电源键只读输入，Reboot 经 EFI 复位钩子返回 Android）、ESP 作为 kShimC Blob、ExitBootServices 时恢复 UFS | ESP/根文件系统的最终位置；UFS 上的 ESP 需要写入许可 |
| 四：Armada 设备移植与 USB 试启动 | 完整系统已进入桌面/游戏模式；显示、触控、磁吸键盘、触控板、Wi-Fi 和桌面音频已实测。配套内核/1594 模块及 USB 持久层工作；1408 音频源码、拓扑和 UCM 已纳入构建，服务失败项已清零 | 完整新镜像/全新可写层冷启动；逐颗扬声器、麦克风增益和开机 APM 超时；挂起恢复与长期稳定性 |
| 五：安装与固化双启动 | 部分：kShimC 菜单已固化到 `boot_b`，但 Linux 启动文件仍内嵌在 boot、根文件系统仍在 U 盘 | 内部 ESP/根/数据布局、最终 boot（不再内嵌 ESP）、升级/恢复方案及多轮双系统启动回归 |

**下一项工作**：确定机内空间方案（§21.2 第 3 项；Linux UFS 只读探测已通过，§19.40）。独立镜像与全新可写层验收已完成（§19.39）；机内双启动的关键缺口和完整剩余清单见第21节。

**剩余工作量**主要在：从源码完整重建新镜像并完成全新可写层/冷启动回归、其余外设和休眠稳定性、内部存储与 ESP 布局、双启动固化及升级/恢复方案。桌面/游戏模式、USB 根文件系统和 Wi-Fi 已有实测记录，不再列为尚未启动。erhai 驱动可继续借鉴，但不运行其重新分区/刷写安装脚本。目前没有可靠的完成百分比估计。

**Android 基线状态**（2026-11-04 起 `boot_b` 为 kShimC 镜像 `9d91e3a9666c9886a761a88401b1b760bd7bdeaeed4a19f54615796dfc30a055`，其中 ColorOS 内核与原 WildKernel 逐字节相同；原镜像 `0e5afd65…` 备份于 `~/oneplus-pad2pro-backup/slot_b/boot_b.img`，恢复用 `fastboot flash boot_b` 该文件，见 §19.39。以下为 2026-11-03 的历史记录）：ColorOS 正常启动，b 槽，`boot_b` SHA-256 当时为 `0e5afd65d6c92bb12c3a319ddcbcfc9ffaac6a68e22c567c4a6ef1cf82705e49`；用户确认解锁、指纹、Wi-Fi、KernelSU/susfs 全部正常。所有实验均为 `fastboot boot` 临时启动，未刷写分区、未改 GPT、未格式化、未刷 OEM ABL；Linux 冒烟 DT 禁用 UFS/PHY，块设备仅 loop0–7。普通重启仍进入原有 Android，尚未固化 kShim 菜单。按本轮用户“更新文档，然后提交”的要求，本方案进度与源码一并提交；原始记录和私有固件仍保留本地。

---

## 0. 设备事实（2026-11-02 通过 adb 采集）

| 项目 | 值 | 来源 |
|---|---|---|
| 型号 / 代号 | OPD2413 / OP615EL1 | `getprop` |
| 系统 | ColorOS `OPD2413_16.0.10.600(CN01)`，Android 16 | `getprop` |
| SoC | SM8750P，`soc_id = 639` | `/sys/devices/soc0/soc_id` |
| 当前槽位 | `_b` | `/proc/bootconfig` |
| Bootloader | **已解锁**（`vbmeta.device_state = unlocked`，`verifiedbootstate = orange`） | `/proc/bootconfig` |
| ABL 选择的 DTB / DTBO 序号 | `dtb_idx = 6`，`dtbo_idx = 5` | `/proc/bootconfig` |
| Root | KernelSU Next，`adb shell` 本身即为 root（`u:r:ksu`） | `id` |
| 内核 | 第三方 WildKernel（GKI 构建，内置 KernelSU Next 与 susfs） | 用户提供 |
| 已装模块 | susfs4ksu、zygisksu、zygisk_lsposed、fas_rs、extreme_gt、droidspaces | `/data/adb/modules` |

**注意事项**

- susfs 会伪造 `ro.boot.flash.locked`、`ro.boot.verifiedbootstate` 等属性（`getprop` 会显示 `locked` / `green`）。**判断设备状态一律以 `/proc/bootconfig` 为准。**
- 设备没有 microSD 卡槽。
- `adb shell` 已经是 root，命令里不要再嵌套 `su -c '...'`。之前因为引号嵌套出过错。

---

## 1. Armada 的工作原理（与本方案相关的部分）

### 1.1 构建

- 系统基于 Fedora bootc（`Containerfile`，`quay.io/fedora/fedora-bootc:44`），以 OCI 镜像的形式构建，通过 ostree 做原子更新和回滚。
- `packages/*`：每个组件由"上游版本（`BASE.env`）+ `patches/` + `build.sh`"构成，按内容哈希单独构建和缓存。内核位于 `packages/kernel`，是 Linux 7.2.6 加上 ROCKNIX 和 Armada 自己的补丁。
- `build_files/*.sh` 负责组装 rootfs，`system_files/` 存放设备数据，包括设备配置、固件、UCM 和 udev 规则。
- `post_process/make-bootimg.sh` 生成 header v0 的 `/KERNEL`（内核后面拼接所有受支持设备的 DTB）。`post_process/finalize-armada-image.sh` 负责放置 OpenABL 刷机包和 EFI 启动文件，并把分区表转换为 MBR。

### 1.2 掌机上的启动链（**一加上不可用**）

```
XBL → OpenABL（替换原厂 abl_a/abl_b）→ SD 卡
    ├─ EFI：systemd-boot + dtbloader → BLS 条目
    └─ 旧路径：/KERNEL（ABL 按 qcom,msm-id/board-id 选择 DTB）
```

一加的 ABL 带有 OEM 签名，SoC 上烧录了安全启动熔丝。**刷入 OpenABL 会导致硬砖。**

### 1.3 运行时识别设备

`system_files/usr/libexec/armada/device-env` 读取 `/sys/firmware/devicetree/base/model`，然后加载 `system_files/usr/lib/armada/devices/<profile>.conf`。

### 1.4 已有的 SM8750 支持

- KONKR Pocket FIT Elite：`packages/kernel/dts/sm8750-konkr-pf-elite.dts`。这是真正的 SM8750（msm-id 0x26a / 0x27f），**也是本机 DTS 的首选模板**。
- AYN Odin 3：`packages/kernel/dts/cq8725s-ayn-odin3.dts`，CQ8725S 变体。

---

## 2. 总体架构

```
电源键 → XBL → 一加原厂 ABL（验签、TEE、bootconfig、dtbo 叠加、加载 ramdisk，全部照常执行）
       → boot_<槽位> 中的 kShimC（伪装成 ARM64 内核镜像，在内核之前运行）
           ├─ [默认 / 超时] Entry 0：WildKernel → ColorOS（原样交还 ABL 准备好的 FDT 和 ramdisk）
           └─ Entry 1：U-Boot → systemd-boot（ESP）→ Armada 内核 + initramfs → ostree rootfs
```

### 2.1 为什么这样设计

- **原厂 ABL 必须完整走一遍。** 启动状态、vbmeta 摘要、防回滚这些信任根信息由 ABL 交给 TEE。如果在 Android 启动之后再用 kexec 跳转，或者用其他 bootloader 去链式加载 Android，都会破坏这条链。kShimC 运行在内核之前，不会碰这些状态。
- **boot 分区只增加几 MB。** kShimC 加 U-Boot 加 DTB 就够了，Armada 的内核和 initramfs 放在 ESP 和 `/boot` 上。
- **Armada 侧几乎不用改。** 直接复用现有的 EFI 启动路径：`armada-efi-update` 已经会把内核、BLS 条目和 `dtbs/qcom/*.dtb` 同步到 ESP（`system_files/usr/libexec/armada/armada-efi-update:52-56`）。

### 2.2 kShimC 的工作原理（已读代码确认）

- `../kShimC/tools/boot_entry.S`：伪造 ARM64 内核镜像头。ABL 把它当内核加载，跳转时 x0 指向 FDT。kShimC 随后把自己搬到板级预留的内存区（`SHIM_LOAD_ADDRESS`）。
- 它从 **ABL 传入的厂商 FDT** 中发现硬件：
  - 屏幕：`cont_splash_region` 加 `qcom,sde-kms`（`src/dt/splash.c`）
  - 按键：`qcom,spmi-pmic-arb` 和 TLMM（`src/dt/spmi.c`、`src/dt/keys.c`）
  - 触摸：只支持 `st,fts`（`src/dt/touch.c`）
- 启动项清单（manifest，见 `src/include/manifest.h`）支持三类条目：
  - 基础 Linux（Entry 0）
  - 带复制标志、可携带独立 DTB 的 Linux / FREE_EXEC 条目
  - UEFI 负载
- 交接给下一级之前，kShimC 会把动过的硬件恢复原状。恢复失败时它会拒绝交接（`src/core/main.c` 中的 `prepare_boot`）。

### 2.3 备选方案（主方案不可行时使用）

| 方案 | 做法 | 代价 |
|---|---|---|
| Recovery 分区方案 | Android 保留在 boot 分区，Armada 的 boot.img 刷进 `recovery_<槽位>`，开机时用 fastboot 菜单里的 "Recovery mode" 进入 | 失去原厂 recovery，每次 OTA 后要重刷；**前提是设备有独立的 recovery 分区【待验证】** |
| kShimC 直接启动 Armada 内核 | Entry 1 直接放 Armada 内核 | 需要给 kShimC 增加 initrd 传递和 `/chosen` 修补，boot 分区容量也可能不够 |
| kShimC → 完整 UEFI（类似 Mu-Silicium） | 用完整的 UEFI 固件代替 U-Boot | 需要有现成的 SM8750 UEFI 移植【待验证】 |

---

## 3. 安全红线

1. **绝对不要刷 `abl_a` / `abl_b`。** 不要运行 Armada SD 卡镜像里的 `openabl/*/flash_abl.sh`。
2. **必须在 Armada 里显式禁止 ABL 自动更新。** `packages/openabl-updater/src/armada-abl-update` 只检查两个条件：设备配置里的 `ARMADA_SOC_CLASS=SM8750`，加上 `soc_id ∈ {639, 706}`。**本机 `soc_id` 正是 639**，所以这两道检查都会通过。目前唯一能拦住写入的，是"认不出原厂 ABL 是 OpenABL 或 ROCKNIX"这一点。处理办法：
   - 在设备配置里加 `ARMADA_ABL_MANAGED=0`，让 `armada-abl-update` 一开头就检查这个开关并退出；
   - 补一个对应的测试；
   - 同时屏蔽 `armada-abl-finalize`。
3. **只要有任何分区不是原厂镜像，就绝对不能重新上锁 bootloader。**
4. **不要刷入比当前版本旧的固件。** OPPO/一加有防回滚机制。
5. **以下分区只读，永远不写**：`persist`、`modemst1`、`modemst2`、`fsg`、`fsc`、`xbl*`、`abl*`。
6. **kShimC 电源菜单里的"进入 EDL"在调试阶段要禁用。**
7. **不需要动 vbmeta。** 本机已经直接启动了第三方内核，证明解锁状态下不改 vbmeta 也能启动修改过的 boot。修改 vbmeta 的标志位在不少机型上会要求清空数据。
8. **`armada-installer` 缩小 userdata 时会清空全部 Android 数据**，见 `armada-installer:251`：*"All Android user data will be erased."*。它并不是无损操作。

---

## 4. 分阶段实施

每个阶段都设有检查点，**上一个检查点没通过，就不进入下一阶段**。

### 阶段一：只读采集与备份（不改设备）

- [x] **1. 基本信息**：已完成，见第 0 节。
- [x] **2. 分区布局与大小**（结果见第 8 节）
  ```sh
  adb shell 'ls -l /dev/block/by-name/ | grep -E " (boot|init_boot|vendor_boot|vendor_kernel_boot|dtbo|recovery|vbmeta|vbmeta_system|abl|userdata)(_a|_b)? "'
  adb shell 'for p in boot_b init_boot_b vendor_boot_b dtbo_b recovery_b vbmeta_b userdata; do f=/dev/block/by-name/$p; [ -e $f ] && echo "$p $(blockdev --getsize64 $f)" || echo "$p absent"; done'
  adb shell 'cat /proc/partitions | grep -E "sd[a-z]$"'
  ```
  这一步要回答四个问题：
  - boot 分区有多大？
  - 有没有独立的 recovery 分区？
  - userdata 在哪个 LUN 上？
  - userdata 后面还有没有其他分区？
- [x] **3. 备份**（已完成。实际做法是用 `adb exec-out dd` 直接流式传到电脑，平板上不落地任何文件，并逐个与设备端 sha256 比对通过）到电脑上的新目录 `~/oneplus-pad2pro-backup/`。设备分区只读不写，用 `dd` 读到 `/sdcard`，再 `adb pull`：
  - 当前槽位的 `boot_b`（即 WildKernel 版本）、`init_boot_b`、`vendor_boot_b`、`dtbo_b`、`vbmeta_b`
  - `/sys/firmware/fdt`，即 ABL 实际交给内核的运行时 FDT
  - 用 `sgdisk --backup`（或 `dd` 前 34 个扇区）备份所有 LUN 的 GPT
  - `persist`、`modemst1`、`modemst2`、`fsg`、`fsc`（用于 IMEI / 基带校准的离线保存）
  - 另外下载同版本 `16.0.10.600(CN01)` 的原厂 OTA 包，从 payload.bin 解出原厂的 boot、init_boot、vendor_boot、dtbo，作为最终退路
- [x] **4. 电脑端分析**（结果见第 8 节）：先 `dtc -I dtb -O dts fdt > runtime.dts`，然后检查：
  - `cont_splash_region` 的地址和大小，`qcom,sde-kms` 里的各类偏移（SSPP、CTL、mixer）
  - `qcom,spmi-pmic-arb` 的版本和寄存器布局，PON（电源键）和 RESIN（音量下键）节点
  - TLMM 的 compatible（例如 `qcom,sun-tlmm` 或 `qcom,sm8750-tlmm`），以及音量上键所在的 GPIO
  - `reserved-memory` 的完整布局（用来选 kShimC 和 U-Boot 的加载地址）
  - `qcom,msm-id`、`qcom,board-id`、`model`、`compatible`
  - 屏幕面板节点（`qcom,mdss-dsi-panel`）：分辨率、是否双 DSI、DSC 参数
  - 触摸芯片型号和总线
  - 另外解包 `vendor_boot` 的 dtb 段，核对第 6 个 DTB 和 dtbo 里的第 5 个叠加层

**检查点 1**：所有备份都已在电脑上保存并校验 sha256，分析结果整理完毕。

### 阶段二：kShimC 适配，只验证启动 Android（不写闪存）

需要的代码改动（以阶段一第 4 步的分析结果为准）：

1. **SoC 支持**：CrDK 目前只有 SM8150/SM8250 的移植数据（`ports/crdk/*-SOURCES.json`），需要补上 SM8750 的，涵盖串口/I2C/SPI 控制器（GENI）、时钟、RPMh、SPMI。PMIC 总线控制器是否支持 v7【待验证】。
2. **按键**：在 `src/dt/keys.c` 的 `QcomFdtLinearTlmmGpioCount()` 里加入本机 TLMM 的 compatible 和 GPIO 数量（目前只有 sm8450 和 sm8550）。
3. **屏幕**：先检查 `CrSplashRead` 能否读懂 SM8750 的 DPU。读不懂时，可以按 `cont_splash_region` 实现一个兜底帧缓冲，但像素格式和行宽（stride）必须从实际扫描输出确认，**不能靠猜**。
4. **内存地址**：kShimC 自身、临时内存页和 U-Boot 的加载地址，都要从 `reserved-memory` 里挑选空闲且不冲突的区域。
5. **新增 defconfig**：`configs/opd2413_defconfig`。第一版只启用按键，不启用触摸，并关闭 EDL 选项。
6. **测试夹具**：把本机的运行时 FDT 加入 `tests/fixtures/`，在电脑上跑通 `dt_test`、`spmi_test` 等单元测试。

验证步骤：

- 先用 `qemu_manifest_ui_defconfig` 在模拟器里跑通菜单和启动项清单流程。
- 再用 MultiBootKernelPatcher 把 kShimC 注入**当前 WildKernel 的 Image**，重新打包成测试 boot.img，然后：
  ```sh
  fastboot boot kshim-test-boot.img   # 只在内存中启动，不写分区
  ```
  本机已通过 T14 和后续多轮实机测试：GKI v4 临时 boot 镜像可使用当前槽位 ABL 准备的 Android 启动上下文，正常进入 ColorOS，见第 10、12、13.3 节。

**检查点 2**：屏幕显示菜单，音量键和电源键都能操作。选择 Android 后完全正常：锁屏解密、指纹、Wi-Fi、KernelSU Next 和 susfs 都正常工作。

### 阶段三：U-Boot 可行性验证（**整条链路风险最大的一环**）

已完成的子检查：

- [x] kShimC 将原始 ABL FDT 交给 U-Boot，U-Boot 从中提取真实内存与保留区，构建自己的受限控制树，保持原 FDT 不变。
- [x] U-Boot 在实机 EL1 显示控制台，使用 RAM 日志，并经校验后直接交回 ABL 已加载的原 WildKernel；Android 功能正常。
- [x] 只读观测 SM8750 UFS、PHY、GCC 和 SMMU 状态，识别 AHB 硬件自动门控。
- [x] 通过 DME_GET 确认 ABL 留下的链路处于 Hibern8；修正版先唤醒链路，再完成一次有效 NOP/DMA 应答，恢复原队列与 Hibern8 后返回 Android。
- [ ] 只读列举 UFS LUN 和容量，读取 GPT 并与备份对照。
- [ ] 启动 EFI 程序，再验证所需的 EFI 存储/文件加载路径。

仍需解决或保留的设计问题：

- **驱动范围**：已核对的固定 U-Boot 版本缺少完整 SM8750 GCC/PHY 支持。当前探测复用 ABL 初始化结果；完整复位/重新初始化与 USB 支持仍需后续移植和实机验证，不能由 NOP 成功推定可用。
- **内存信息怎么传（观测路径已验证）**：kShimC 仍传原始 ABL FDT；U-Boot 从中提取真实内存/保留区，生成受限控制树。后续 EFI 和 Armada 设备树仍必须正确继承这些真实信息，不能用主线 DTB 中的静态占位布局覆盖。
- **DTB 怎么更新**：U-Boot 要设置 `fdtfile`，从 ESP 读取 `dtbs/qcom/sm8750-oneplus-pad2pro.dtb`。原因是 dtbloader（TravMurav/dtbloader）靠 SMBIOS 硬件 ID 匹配设备，没有这台平板的条目。如果不这样设置，打包在 boot 分区里的 DTB 永远不会随 Armada 的更新而更新。

验证方式：继续把 U-Boot 作为 kShimC 的 Entry 1，用 `fastboot boot` 临时启动。当前负载尚未接入通用 UFS/SCSI/块设备接口，不能直接把 `ufs init` 或 `part list` 当作已可用的验证命令。先完成受限只读 LUN/GPT 探测，再接入经过审查的存储与 EFI 路径。完整复位/重新初始化路径的 GCC、PHY、TLMM 支持仍未完成。

**检查点 3**：U-Boot 能稳定读到存储设备，内存布局正确，并且能启动 EFI 程序。**如果这一步失败，回到第 2.3 节换备选方案。**

### 阶段四：Armada 设备移植（根文件系统先放 USB-C 硬盘）

**内核**

- 新建 `packages/kernel/dts/sm8750-oneplus-pad2pro.dts`，以 KONKR 的 DTS 为模板，并接入 dts 补丁流程。
- `qcom,msm-id`、`qcom,board-id`、`reserved-memory`、供电轨，都照抄本机运行时 FDT 里的值。
- 调试初期保留 `cont_splash_region`，用 simplefb 输出画面。
- 各硬件的处理：
  - 屏幕：需要写面板驱动（从厂商 DT 移植初始化序列、时序和 DSC 参数），这是最难的一项
  - 背光
  - 触摸：确认芯片后找主线驱动
  - 充电：pmic_glink + battmgr；SuperVOOC 快充基本不能用
  - USB-C：ucsi
  - Wi-Fi：WCN7850，ath12k
  - 蓝牙
  - 音频：多个功放加 AudioReach 拓扑，再写 UCM

**固件**

- 从本机分区提取一加签名的固件：adsp、cdsp、`gen80000_zap.mbn`、vpu、功放固件、battmgr 的 jsn 等。
- 放到 `system_files/usr/lib/firmware/qcom/sm8750/oneplus/pad2pro/`，DTS 里的 `firmware-name` 指向这个目录。

**用户态配置**

- 已完成设备识别、`oneplus-pad-2-pro.conf` 和 `ARMADA_ABL_MANAGED=0`，并在 updater/finalizer 中显式排除本机，见第 10.4 节。这些保护已提交并通过文件模拟测试，不代表 Armada 已经启动。
- `system_files/usr/lib/armada/supported-dtbs` 加入新的 DTB 名。
- 后续补全设备配置；屏幕方向要实测，不要照抄 KONKR 的 `right`。
- 传感器：用 `packages/armada-ssc/capture.sh` 采集校准数据。
- 平板没有内置手柄，游戏模式需要外接控制器。

**检查点 4**：从 USB 硬盘启动 Armada 后，屏幕、触摸、Wi-Fi、GPU 和充电都能正常工作，而且 `armada-abl-update` 确认会拒绝执行。

### 阶段五：固化双启动

1. **（可选）把 Armada 装到内部存储。** 前提条件：
   - Android 数据已完整备份，GPT 已备份；
   - 用 `sfdisk -l` 确认 userdata 后面没有其他分区，否则 `armada-installer:249` 会把它们删掉。

   安装后 Android 需要格式化数据。注意 installer 的错误恢复提示写的是"在 ABL 里选 UNINSTALL CFW"，这是 OpenABL 的菜单，一加上没有。分区表的恢复要靠阶段一备份的 GPT。
2. **打包最终的 boot 镜像**：WildKernel（Entry 0）加 U-Boot 和 DTB（Entry 1）加 kShimC。
3. **刷入当前槽位**：
   ```sh
   fastboot getvar current-slot
   fastboot flash boot_<当前槽位> boot_kshim_dualboot.img
   ```

**检查点 5**：重启 10 次以上，两个系统都能稳定进入，Android 的所有功能不受影响。

---

## 5. 日常维护

- **ColorOS OTA**：OTA 会把新的 boot 写进另一个槽位，然后切换过去，kShimC 菜单随之消失，但 Android 照常开机，不会变砖。你本来每次 OTA 后都要重刷 WildKernel，在同一步里用新的 WildKernel Image 重新打包 kShimC 即可。之后可以写一个 root 脚本，在 OTA 后自动打包并写入新的活动槽位。
- **Armada 更新**：内核、initramfs、BLS 条目和 DTB 都经由 ESP 更新，不需要动 boot 分区。前提是阶段三里的 `fdtfile` 已经设置好。
- **出问题时的恢复**：
  - kShimC 或 U-Boot 出问题：`fastboot flash boot_<槽位>` 刷回备份的 WildKernel boot，或原厂 boot。
  - 分区表出问题：用备份的 GPT 恢复。

---

## 6. 对 `../ONEPLUS_PAD2_PRO_PORTING_AND_DUALBOOT.md` 的修正

| 原文说法 | 修正 |
|---|---|
| armada-installer 能"无损缩小" userdata | 会清空全部 Android 数据（`armada-installer:251`） |
| 阶段一要用 `--disable-verity --disable-verification` 刷 vbmeta | 不需要，而且有风险 |
| Armada 更新"永远不需要重刷 boot_a" | 只对内核和 initramfs 成立。DTB 要靠 U-Boot 从 ESP 读取才会更新 |
| 固定刷写 `boot_a` | 应该刷当前槽位。本机当前是 `_b` |
| 没有提到 OTA | OTA 会覆盖 kShimC，之后需要重新打包 |
| kShimC "只需 3 处修改" | 还要补齐 CrDK 的 SM8750 移植、选定内存地址，并验证 SPMI v7 |
| 默认主线 U-Boot 可用 | 尚未验证，是整条链路风险最大的一环，另外还有内存信息怎么传的问题 |
| "bootconfig 注入 FDT" | header v4 的 bootconfig 是追加在 ramdisk 后面的 |
| 屏幕方向 `right` | 抄自 KONKR，需要实测 |

---

## 7. 下一步

进入阶段三的 SM8750 U-Boot 移植；先解决第 11 节的驱动和观测路径缺口，再制作 Entry 1 的临时启动镜像。保持 T14 为可用的 Android 菜单基线。

---

## 8. 阶段一采集结果（2026-11-02）

文件位置：

- 备份：`~/oneplus-pad2pro-backup/`（权限 0700），里面有 `SHA256SUMS`
- 解包和分析结果：`~/oneplus-pad2pro-work/extract/`
- 脚本：`~/oneplus-pad2pro-work/scripts/bootimg_info.py`，用来解析 boot、vendor_boot（v3/v4）和 dtbo
- 原厂全量 OTA 包：`~/Documents/Android_ROMS/OPD2413_16.0.10.600(CN01)/payload.bin`。metadata 里没有 `pre-build` 字段，说明是全量包；`ota-type=AB`，版本与平板当前系统一致

### 8.1 分区与备份

| 分区 | 位置 | 大小 | 已用 / 说明 |
|---|---|---|---|
| `boot_a/b` | sde12 / sde48 | 96 MiB | boot_b 是 header v4，只放了 WildKernel 6.6.118 的 Image（36.6 MiB），**剩余约 59 MiB**，没有 AVB 签名 |
| `init_boot_a/b` | sde30 / sde65 | 8 MiB | 只有通用 ramdisk（3.1 MiB） |
| `vendor_boot_a/b` | sde23 / sde59 | 96 MiB | header v4，有 13 个 DTB；bootconfig 里写着 `usbcontroller=a600000.dwc3` |
| `dtbo_a/b` | sde16 / sde52 | 24 MiB | 16 个叠加层，id/rev 全为 0（ABL 按内容匹配） |
| `recovery_a/b` | sde27 / sde62 | 100 MiB | **存在**，header v4，**只有 ramdisk、没有内核**（kernel=0）。ABL 在 recovery 模式下会不会使用 recovery 镜像自带的内核【待验证】，所以 2.3 节的 recovery 方案前提存疑 |
| `vbmeta_a/b` | sde15 / sde51 | 64 KiB | 不需要改动 |
| `userdata` | sda15 | 约 460 GiB | **是 sda 上的最后一个分区**，后面只剩备份 GPT 占用的 40 个扇区（512 B 单位） |
| `abl_a/b` | sde8 / sde44 | — | **禁止写入** |

- **UFS 逻辑扇区是 4096 字节**，处理 GPT 时要按这个算。
- 这台平板只有 Wi-Fi，没有 `modemst1/2`、`fsg`、`fsc`，对应的是 `nonmodemst1/2`。
- 已备份：两个槽位的 boot、init_boot、vendor_boot、dtbo、recovery、vbmeta；设备专属分区 persist、nonmodemst1/2、devinfo、param、oplusreserve1–5、spunvm、vm-persist；运行时 FDT；6 个 LUN 的主 GPT 和备份 GPT（签名均校验为 `EFI PART`）。

### 8.2 运行时 FDT（ABL 实际交给内核的那份）

- `model = "Qualcomm Technologies, Inc. Sun MTP,erhai-O DVT"`，`compatible = "qcom,sun-mtp", "qcom,sun", ...`
- `qcom,msm-id` 与 KONKR Pocket FIT Elite 完全相同（0x26a / 0x27f 系列）
- **`qcom,board-id = <0x40008 0x0>`**（KONKR 是 `0x2000b`）
- 项目号 24926，内部代号 erhai

### 8.3 kShimC 关心的硬件

**按键**：基本可以直接用

- 电源键：PMK8550 PON 模块 `pon_hlos@1300` 下的 `qcom,pmk8350-pwrkey`
- 音量**上**：同一个 PON 模块下的 `qcom,pmk8350-resin`，`linux,code = 0x73`。注意这里 resin 是音量上，和常见机型相反
- 音量**下**：`gpio-keys` 节点，接在 PM8550（SID 1）`pinctrl@8800`（`qcom,pm8550-gpio`）的 GPIO 6 上，低电平有效
- kShimC 已经识别 `qcom,pmk8350-pwrkey/resin`（`src/drivers/keys.c:263,271`），`qcom,pm8550-gpio` 也符合它的 PMIC GPIO 匹配规则
- TLMM 的 compatible 是 `qcom,sun-tlmm`，不在 kShimC 的线性 TLMM 白名单里，但这台机器的按键不经过 TLMM

**SPMI**：`spmi0_bus` 位于 `0xc42d000`（`spmi1` 被禁用）。CrDK 里有 `pmic_arb_v7.c` 和 `pmic_arb_v8.c`，具体是哪个版本要从硬件寄存器读取后确认。

**屏幕**

- `cont_splash_region` 位于 `0xfc800000`，大小 `0x2b00000`（43 MiB）
- 显示控制器 `mdp_phys` 位于 `0xae00000`，大小 `0x93800`
- 面板：`mdss_dsi_panel_XN242_P_D_A0024_vid`（"XN242 p d dsc video mode panel"），**双 DSI**（两个控制器、两个 PHY），每路 1696×2400，**合计 3392×2400**，视频模式，24 bpp；**DSC 1.2**，8 bpp，切片 848×20；显示拓扑 `<4 4 2>`，即 4 个混合器、4 个 DSC、2 个接口；默认 120 Hz
- 物理尺寸 274×193 mm，**原生横屏**。所以 Armada 的 `ARMADA_PANEL_ORIENTATION` 大概率应该是 normal，不是 `right`
- 背光：外置 KTZ8868，走 I2C（`bl_ctrl_external`），最大等级 0x7ff，复位 GPIO 是 TLMM 98
- ABL 日志确认：先检测到 XN242 面板，再显示 1080×1920 的 logo

**kShimC 屏幕部分已确认的不兼容（静态分析得出）**

| 项目 | 本机 | kShimC（`src/dt/splash.c`）当前上限 |
|---|---|---|
| `qcom,sde-sspp-off` | 10 个（4 个 vig + 6 个 dma） | 容量 8，dma 最多 4 个，超出直接返回 -1 |
| `qcom,sde-mixer-off` | 12 项（8 个真实 + 4 个 `0xf0f` 占位） | 容量 6，超出返回 -1 |
| `qcom,sde-ctl-off` | 6 个 | 容量 8，没问题 |
| 显示拓扑 | 4 个混合器 | `CrSplashRead` 只支持 1 或 2 个管线，且不接受一个管线扇出到多个混合器 |

**重启后补充采集的结果**（日志存放在 `~/oneplus-pad2pro-work/logs/`）

- dmesg 里有 `default topology: lm: 4 comp_enc:4 intf: 2` 和 `cont_splash enabled in 1 of 1 display(s)`，说明 ABL 确实把开机画面交给了内核。
- DRM debugfs 的 `rm_status`（这台机器的 debugfs 挂载在一个随机路径下，要用 `mount | grep debugfs` 查找）显示，接管时占用了 4 个 LM、1 个 CTL、4 个 PINGPONG、4 个 DSC 和 2 个 INTF。
- 当前日志级别下，内核不打印 ABL 开机画面用的是哪几号管线。Android 上也没有 `/dev/mem`，读不到 ABL 写的寄存器。
- **结论**：混合器最大线宽是 2560（`mixer-linewidth = 0xa00`），屏幕横向被切成 4 份、每份 848 像素，每个 LM 都需要自己的输入。所以 ABL 的开机画面**至少用了 4 路管线（或 4 个多矩形 rect）**。kShimC 需要把 `CrSplashRead` 扩展到 4 个 LM、每个 LM 接一路管线，并校验这 4 路是同一块帧缓冲横向切出的连续片段（行宽和像素格式一致）。具体用了哪几号管线由读取器在运行时扫描得到。另外要提供一个按 `cont_splash_region` 起始地址、3392×2400、32 bpp 来画的调试兜底模式，用于第一次上机时观察。

**触摸**：Novatek NT36536（`novatek,nf_nt36536`），kShimC 不支持。第一版菜单只用按键操作。

### 8.4 内存

- 保留内存共 64 个节点，按地址排好的列表在 `~/oneplus-pad2pro-work/extract/fdt/reserved.txt`
- 高地址区域的主要占用：
  - `tz_merged` 位于 0xd8000000，6 MiB
  - `trust_ui_vm` 位于 0xf3800000，68 MiB
  - `oem_vm` 位于 0xf7c00000，76 MiB
  - `splash_region` 位于 0xfc800000，43 MiB
  - `llcc_lpi` 位于 0xff800000，8 MiB
- kShimC 自身、临时内存页和 U-Boot 的加载地址，要在阶段二结合 ABL 实际的内核加载地址再确定

### 8.5 对 Armada 移植有用的信息

- 触摸芯片：NT36536；背光芯片：KTZ8868（I2C）
- USB 控制器：`a600000.dwc3`
- 内核命令行里有 `video=vfb:640x400`、`console=ttynull` 和 `qcom_geni_serial.con_enabled=0`，说明串口控制台被关闭了，调试只能靠屏幕或 USB

### 8.6 原厂镜像与当前设备的对比

- 工具：payload-dumper-go 2.1.0，位于 `~/oneplus-pad2pro-work/tools/`，下载后已用官方 sha256 校验。
- 已解出到 `~/oneplus-pad2pro-work/stock/`：boot、init_boot、vendor_boot、dtbo、recovery、vbmeta、vbmeta_system、vbmeta_vendor、modem、dsp、bluetooth。vendor 和 odm 等需要时再解。

| 分区 | 设备 slot_b 与原厂镜像对比 |
|---|---|
| vendor_boot / dtbo / recovery / vbmeta | **一致** |
| boot | 不同：设备上是 WildKernel（`6.6.118-android15-OP-WILD`，内置 KernelSU 和 susfs）；原厂是 `6.6.118-android15-8-gc4127a25dcf3-ab15863337-4k` |
| init_boot | **不同**：设备上是 **2025-12 旧版 ROM 的 init_boot**（os_version 0x1e00019c，`build.date` 是 2025-12-09），而且被 KernelSU 以 LKM 方式打过补丁（多了 `init.real`、`kernelsu.ko`、`ksu_config=bundled=1`）。原厂当前版本是 2026-09 |

- 当前系统运行正常，本方案不改动 init_boot。但要恢复成纯原厂状态时，**init_boot 也必须一起刷回原厂镜像**。
- slot_a 上的分区与当前 OTA 版本都不一致，是更早的版本。不要切换到 slot_a。

### 8.7 ABL 的 DTB 选择逻辑（已确认）

- vendor_boot 里的 13 个底层 DTB，board-id 都是 0，ABL 按 msm-id 选中第 6 个：`dtb_06`，"SunP v2 Alt. Thermal Profile SoC"
- dtbo 里的 16 个叠加层，board-id 都是 `0x40008`，ABL 再从中选中第 5 个：`dtbo_05`，"Sun MTP,erhai-O DVT"。这两者合并后就是运行时 FDT
- 在 kShimC → U-Boot 这条链路上，Armada 的主线 DTB 是 kShimC 或 U-Boot 直接传给内核的，**不经过 ABL 选择**。所以 Armada DTS 里的 msm-id 和 board-id 在这条链路上不关键，但照抄过来也无妨

### 8.8 阶段一结论

阶段一全部完成。进入阶段二（kShimC 适配）时，已知需要改的地方有：

1. `src/dt/splash.c`：管线容量至少扩到 10，dma 类型至少扩到 6；混合器容量至少扩到 8，并过滤 `0xf0f` 占位项
2. `CrSplashRead`（CrDK 补丁 0002）：支持 4 个 LM、每个 LM 一路管线的拼接方式，外加调试兜底模式
3. 根据 ABL 实际的内核加载地址和 8.4 节的保留内存，确定 kShimC、临时内存页和 U-Boot 的加载地址
4. 新增 `configs/opd2413_defconfig`：只启用按键，关闭 EDL 和触摸
5. 把运行时 FDT 加入 `tests/fixtures/`，补充 splash、keys、spmi 的单元测试
6. 确认 SPMI 控制器是 v7 还是 v8（运行时读硬件版本寄存器）

---

## 9. 阶段二早期进度（历史记录，最新状态见第 10 节）

以下为首次离线构建时的记录。当前 kShimC 基线提交为 `4d89a17`，本轮后续修复尚未提交。

### 9.1 关键发现：DPU v12 改变了管线到混合器的连接方式

这一节已对照主线 Linux v7.2 的 `dpu_hw_ctl.c`、`dpu_hw_lm.c` 和 `catalog/dpu_12_0_sm8750.h` 核实，参考文件存放在 `~/oneplus-pad2pro-work/ref/linux-dpu/`。

- SM8750 是 **DPU 12.0**。管线到混合器的连接不再写在 CTL 的 `CTL_LAYER*` 里，而是写在混合器（LM）里：
  - 背景源选择 `LM_BG_SRC_SEL`，偏移 0x14，复位值 `0xc0c0` 表示没有接任何源
  - 每个混合阶段的前景源选择 `LM_BLENDn_FG_SRC_SEL`，位于阶段偏移 + 4。每个源占 1 字节，编码为 `类型<<6 | rect<<4 | 编号`，类型 0 = DMA，1 = VIG，3 = 未使用
- CTL 里改用两个位图：`CTL_PIPE_ACTIVE`（0x12c，DMA0–5 对应 bit 0–5，VIG0–3 对应 bit 16–19）和 `CTL_LAYER_ACTIVE`（0x130，bit n 对应 LM_n）
- 管线（SSPP）寄存器布局到 v13 才变，v12 与旧版相同
- 下游 DT 里的偏移量以 `mdp_phys` = 0xae00000 为基准，主线以 0xae01000 为基准，所以下游偏移比主线多 0x1000
- 结论：CrDK 原有的 `CrSplashRead` 在这台机器上一定会失败，必须专门写一条 v12 的检测路径

### 9.2 已完成的改动

| 文件 | 内容 |
|---|---|
| `ports/crdk/patches/0003-splash-dpu-v12.patch`（新增，并登记到 `UPSTREAM.json` 和 `ports/crdk/CMakeLists.txt`） | 新增 `CR_SPLASH_DPU_V12` 路径：按 `CTL_LAYER_ACTIVE`、`CTL_PIPE_ACTIVE` 和各混合器的 SRC_SEL 解析连接关系，每个活动混合器只接受一个单 rect 管线；最多 8 个切片，**按缓冲区地址排序拼接**，并要求各切片高度、行宽、像素格式一致且在内存中首尾相连。旧版（v12 之前）的路径行为不变 |
| `src/dt/splash.c` 和 `src/include/qcom_dt.h` | 新增 `kshim_dt_splash_flags()`；v12 模式下管线容量扩到 16、DMA 最多 6 个，混合器列表去掉末尾的 `0xf0f` 占位项，混合阶段偏移从 `qcom,sde-mixer-blend-op-off` 读取 |
| `src/dt/resources.c` 和 `src/Kconfig` | 新增 `CONFIG_KSHIM_SPLASH_DPU_V12` 开关，默认关闭 |
| `tests/fixtures/opd2413-sde.dts` 和 `tests/opd2413_splash_test.c`（新增） | 精简后的本机开机画面相关 DT（不含序列号等身份信息），加上用真实寄存器偏移做的端到端测试：覆盖正常的 4 切片、切片顺序与混合器顺序不同、5 切片，以及 12 种拒绝情况，并验证整个过程不写任何寄存器 |

验证结果：宿主机测试 **20/20 全部通过**（原有的 19 个加上新增的 1 个）。做了 4 项变异测试（去掉地址排序、允许一个混合器接多个源、跳过 `CTL_PIPE_ACTIVE` 交叉检查、接受只显示背景色的混合器），**4 项全部被测试抓到**。基线构建时发现本机缺少 libasan 和 libubsan，已经用 pkexec 安装。

### 9.3 加载地址规划需要的数据（只读采集）

- ABL 把内核 Image 加载在 **0xa8000000**（`/proc/iomem` 显示 Kernel code 从 0xa8010000 到 0xa9ddffff，Kernel data 从 0xaa280000 到 0xaa53ffff；Image 头的 `image_size` 是 0x2540000）
- ramdisk 位于 **0xb6b48000–0xb7ffe964**（取自 `/chosen`）
- 内核开启了 KASLR，`/chosen` 里有 `kaslr-seed`
- 现有的 `tools/build_hdk.py` 走的是 **standalone 模式**（kShimC 本身就是整个内核，内嵌 UEFI 固件），不适用于我们。我们需要**启动项清单模式**，靠 MultiBootKernelPatcher 把 kShimC 挂在 WildKernel 前面。
  - 这个打包工具本地没有，需要克隆 Project-Aloha/DualBootKernelPatcher
  - `tests/manifest_qemu.py` 里有一段注释提到：打包工具目前会让附加条目沿用基础条目的 DTB 引用，**无法给附加条目单独指定 DTB**。这直接影响 U-Boot 条目怎么拿到 DTB，必须读打包工具源码确认

### 9.4 打包工具与加载地址

- 打包工具：Project-Aloha/DualBootKernelPatcher 的 **`feature/2610` 分支**（与 kShimC 同名的分支，带启动项清单格式）。`main` 分支是旧版的双启动工具，不能用。已用 `git worktree` 检出到 `~/github/MultiBootKernelPatcher`，这是 kShimC 测试脚本默认查找的路径，用户原来那份 clone 仍停留在 `main`。
- 打包后镜像的第一条指令是 `BL kShimC`。kShimC 在挂载模式下**原地运行**：它是位置无关的，启动时自行修正 GOT，BSS 和栈都放在自带的 4 MiB 槽位里。所以**不需要为 kShimC 另选加载地址**。只有 `CopyTo` 类型的启动项（例如阶段三的 U-Boot）才需要选定固定地址。
- 启动 Android 时，kShimC 从基础镜像 +4 处进入，执行的正是原内核的第二条指令 `b primary_entry`，同时原样交回 ABL 传来的 x0（FDT）。

### 9.5 新增改动

| 文件 | 内容 |
|---|---|
| `CMakeLists.txt` | **修复一个原本就存在的问题**：picolibc 的 AArch64 汇编字符串函数（如 strlen、memcpy）用 `ldp` 做非对齐读取。在 Release（`-O3`）构建下会选中它们，因为汇编文件既没有 `-mstrict-align`，也拿不到 `-Os`。kShimC 在 MMU 打开之前就解析 FDT，那时所有内存访问都按 Device 类型处理，非对齐访问会直接引发对齐错误。现在给 C 和汇编都定义 `__PREFER_SIZE_OVER_SPEED`，统一改用通用的 C 实现，并给汇编补上 `-mstrict-align -march=armv8-a` |
| `configs/opd2413_defconfig`（新增） | 按键加 SPMI、v12 开机画面；关闭触摸、多核（SMP）、UEFI 测试菜单、启动横幅；日志级别设为 NONE |

**这个对齐问题是怎么发现的**：仓库自带的 QEMU 回归 `tests/manifest_qemu.py`，在**未改动的 `feature/2610`** 上同样跑不出任何输出。用 `-d int` 抓到的异常是：`strlen` 处的数据中止（ESR 0x96000021，对齐错误），出错地址 0x48000044 正好落在 DTB 里，随后 CPU 跳到 0x200，陷入无限的未定义指令异常。CI 里只编译、不执行这个回归，所以一直没暴露。**真机上同样会触发**，因为 ABL 交接时 MMU 是关闭的。

### 9.6 验证结果（全部离线完成，未上机）

- 宿主机测试：20/20 通过
- QEMU 清单协议回归：`manifest_qemu.py` 6/6 通过（修复前在原版代码上 0/6），`manifest_ui_qemu.py` 2/2 通过
- 运行时构建：opd2413 和 qemu_manifest_serial 均通过（正好 4 MiB，无未解析符号）。opd2413 的代码加 BSS 为 4,159,699 字节，**离 4 MiB 上限只剩约 34 KB**
- hdk8250 在挂载模式下超出 4 MiB 槽位。**原版代码同样超出**，与本次改动无关。可能是本机 GCC 16 生成的代码比 CI 的 Ubuntu GCC 更大
- 打包：真实 WildKernel Image 加 opd2413 运行时打包后，再用 UnpackTool 解包，BaseImage 和 Shim 都与原件逐字节一致。镜像头：第一条指令 `0x94950000`（`BL +0x2540000`），第二条原指令保留，`image_size` 为 0x29400c4，kShimC 偏移 0x2540000（等于原内核的 `image_size`，所以内核清零 BSS 时不会覆盖 kShimC）
- 测试镜像：`~/oneplus-pad2pro-work/stage2/kshim-test-boot.img`，41.3 MiB，sha256 为 `03edfcb14a0a2f6ebbc028dc8353d9e68e98b024e660e5dc28d436d42d0cd738`。与原 boot_b 相比，头部只有 `kernel_size` 一个字段不同
- 新增的工作脚本：`~/oneplus-pad2pro-work/scripts/repack_boot_v4.py`
- 本机新装的依赖：aarch64 交叉 gcc 和 binutils、qemu-system-aarch64-core、glibc-static、libasan、libubsan；kconfiglib 装在 `~/oneplus-pad2pro-work/venv`

### 9.7 下一步：上机临时启动（需要用户确认）

```sh
adb reboot bootloader
fastboot boot ~/oneplus-pad2pro-work/stage2/kshim-test-boot.img   # 只放进内存，不写任何分区
```

- 预期结果：屏幕上出现 kShimC 菜单，只有一项 "ColorOS"，8 秒后自动进入；进入后 ColorOS、KernelSU 和 susfs 都正常
- 可能出现的失败：
  - ABL 拒绝对 GKI v4 镜像执行 `fastboot boot`
  - 开机画面读取失败：kShimC 会退回串口菜单（`src/core/platform.c` 中的 `serial_select`），按键照样能用，8 秒超时后自动启动 ColorOS。表现是屏幕停在 ABL 的 logo，约 8 秒后正常进入系统。**这种情况不会卡死**，但说明 v12 读取路径还要调试
  - kShimC 卡住，屏幕停在 logo
- 恢复办法：长按 电源键 + 音量上键 强制重启，平板会从闪存里原封未动的 boot_b 正常启动


---

## 10. 阶段二检查点通过（2026-11-02，T10–T14）

### 10.1 实机调试结论

接续日志中的 T9，最终通过的是 **T14**，全部使用 `fastboot boot` 临时启动。

| 测试 | 结果与意义 |
|---|---|
| T10：15 秒原始按键诊断 | 只构建出一个按键描述符：音量下。PON 电源键和音量上键没有被识别。此前“音量键被当作确认”的推测未得到证实 |
| T11：修复 PON 多基址解析 | 三键都被识别。PON 0x1310 空闲值 0x10；音量上按下变成 0x50，电源按下变成 0x90；PM8550 GPIO6 空闲 1、按下 0。三键按下/松开事件完整、动作正确 |
| T12：8 秒正常菜单 | 快速短按未产生菜单事件，8 秒后正常自动启动 Android |
| T13：30 秒菜单、按住约 1 秒 | 音量下/上成功切换焦点，取消自动倒计时。菜单停留 44.507 秒后收到电源确认进入 ColorOS；用户随后确认碰过实体电源键，排除了“自动重启”的误判。最长帧 13 毫秒 |
| T14：加快采样后的正常短按 | 音量下、音量上、电源确认全部正常，用户确认画面和操作均正常。日志显示 5.610 秒时确认 ColorOS，`autoboot_cancelled=1` |

**两个根因：**

1. `src/dt/spmi.c:kshim_dt_spmi_reg()` 只接受一个寄存器元组。本机 PMIC 的 `#address-cells=1`、`#size-cells=0`，所以 PON 的 `reg=<0x1300 0x800>` 是 **HLOS 和 PBS 两个基址**，不是基址加长度。原实现拒绝了这个属性，导致整个 PON 按键组丢失。现在允许完整的元组列表、校验所有元组，并返回第一个外设基址。旧的双单元 downstream 格式保持支持。
2. LVGL 默认的按键读取定时器约每 33 毫秒执行一次，再要求两次采样去抖，会漏掉快速短按。已用宿主机 25 毫秒短按测试复现：修复前焦点移动断言失败，修复后通过。现在 keypad 使用 event 模式，由约 5 毫秒一次的平台循环主动调用 `lv_indev_read()`，绘制定时器保持独立。

另外修复了串口兜底把 `QcomKeysPoll()` 的 `NO_EVENT/OK=0` 当作事件成功的问题，正确事件返回值是 `QCOM_KEYS_EVENT=1`。

SPMI 硬件版本寄存器实测为 **0x70020000（v7）**。日志中的 “ABI version 5” 是 CrDK 的枚举值，不是硬件 v5。

### 10.2 代码、测试与 Android 检查

kShimC 新增 `tests/fixtures/opd2413-keys.dts`，只包含脱敏的 SPMI/PON/GPIO 按键布局。测试覆盖真实三键布局、独立动作和松开事件、无效/截断寄存器列表、诊断阶段屏蔽菜单输入、25 毫秒短按。

- 宿主机 ASan/UBSan 回归：**20/20**。
- QEMU Manifest 协议回归：**6/6**；真实 LVGL 菜单交接回归：**2/2**。
- OPD2413 runtime 构建通过，4 MiB，无未解析符号。
- 新增可复现配置：`../kShimC/configs/opd2413_debug_defconfig`。它保留调试色条、ramoops 和事件日志，使用 MINIMAL、128 KiB 双部分绘制缓冲、单 CPU、无触摸。按键诊断窗口默认关闭。
- T13 后用户确认锁屏解锁、指纹、Wi-Fi、KernelSU/susfs 全部正常；T14 用户再次确认短按切换和确认启动正常。
- T14 实机 `power caps=0x2`，只有 PSCI 关机能力，没有 EDL。
- 测试前和 T12/T14 后读取的 `boot_b` SHA-256 均为 `0e5afd65d6c92bb12c3a319ddcbcfc9ffaac6a68e22c567c4a6ef1cf82705e49`。本轮没有执行任何设备分区写入。

**检查点 2 通过。** 当前仍是临时镜像，不会在下一次普通重启时保留菜单。

### 10.3 已保存的可用基线

目录：`~/oneplus-pad2pro-work/stage2k/`。

- `t14-boot.img`：可直接用于 `fastboot boot` 的基线，30 秒自动启动 ColorOS。
- SHA-256：`6a18d5dd6cb0e0e1daba8f879d6bc0611d4568d82d382373cd49862a231b1a29`。
- `t14-Image`、`t14-shim.bin`、`t14.cfg`、`t14.config`：打包材料。
- `t10-result.log` 至 `t14-result.log`：各轮实机诊断。
- `kshim-changes.patch`：相对 `4d89a17` 的本轮修改，包含新增夹具和 debug 配置。
- `host-tests.log`、`short-test-before.log`、两份 QEMU validation JSON：通过结果和短按问题修复前的失败证据。
- `validation.json`、`SHA256SUMS`：实机验证状态及完整性信息。
- `build.sh`：重建脚本，只操作电脑文件，自动限制编译线程以留出至少一个 CPU 核；打包后会解包并逐字节比对内核和 Shim。可通过 `KSHIM_SRC`、`PATCHER_SRC` 指定源码目录。
- 已实际执行一次完整重建；`rebuilt-boot.img` 与实机 T14 的 SHA-256 完全相同，重建过程见 `rebuild.log`。

例如，临时复测现成基线：

```sh
adb reboot bootloader
fastboot boot ~/oneplus-pad2pro-work/stage2k/t14-boot.img
```

### 10.4 Armada 的 OEM ABL 保护已实现

- 新增 `devices/oneplus-pad-2-pro.conf`，配置 `ARMADA_ABL_MANAGED=0`。
- `device-env` 同时识别未来主线 model `OnePlus Pad 2 Pro` 和本机 ABL 的精确 model 字符串，并输出该策略字段。
- `armada-abl-update` 在读取/识别 ABL 分区前退出；`armada-abl-finalize` 在解析部署或调用更新器前退出。
- 两个脚本还按 `oneplus-pad-2-pro` 设备 ID 显式排除本机，即使策略字段意外缺失或为 1，也不会启用更新。
- `tests/abl-update-test.sh` 已添加禁止写入和 finalizer 不调用 writer 的用例，全部通过；`tests/bootimg-regen-test.sh` 也通过。**这些测试仅使用电脑上的文件模拟分区。**

这只是用户态识别及 ABL 保护，不表示 Armada 内核/DTS、外部启动或安装已完成。

## 11. 阶段三源码核对：U-Boot 仍需要移植

以下保留制作观测负载前的源码核对结论；当时尚未在平板上运行 U-Boot。后续已经通过的实机子检查见第 12、13 节，当前总体进度以上方进度表为准。2026-11-02 拉取了 U-Boot 主线提交 `ee9fc27f2a2a43f253a6fe18d7a6f7c4eb0cda3b`（2026-11-01）。该固定版本仍没有完整 SM8750 驱动支持，后续 NOP 实验复用了 ABL 已初始化的硬件状态，没有完成存储或 EFI 验证。

| 组件 | 核对结果 | 后续工作 |
|---|---|---|
| GCC 时钟 | 没有 `clock-sm8750.c` 或 SM8750 GCC 匹配项，已有 SM8650 | 从 SM8750 的 Linux 驱动/厂商资料移植 UFS、USB 所需时钟、复位和电源域；不要套用 SM8650 的寄存器偏移 |
| UFS PHY | `phy-qcom-qmp-ufs.c` 没有 `qcom,sm8750-qmp-ufs-phy`，也不匹配本机厂商 `qcom,ufs-phy-qmp-v4-sun` | 移植 SM8750 PHY 初始化表、寄存器布局和必要的供电/复位控制 |
| TLMM | 主线 U-Boot 有 SM8650，但没有 SM8750/Sun 的独立驱动 | 处理存储 reset、USB/调试相关引脚；PON 菜单按键无需依赖 TLMM |
| UFS 主机 | 有通用 `qcom,ufshc` 匹配；本机主机位于 0x1d84000，PHY 位于 0x1d80000 | 匹配通用 compatible 不代表 SM8750 可用；仍需核对主机版本、时钟、PHY、DMA/IOMMU 和供电 |
| 真实内存信息 | `board_fdt_blob_setup()` 已支持“使用内部 DT，同时从外部 ABL FDT 获取内存信息” | 首选让 kShimC 仍传原始 ABL FDT，U-Boot 内置主线 DT。验证内部 `/memory` 不会以错误静态值覆盖真实内存 |
| Gunyah | 仓库有 Dragonwing 用的 exit-Gunyah 配置 | 本机不能未经验证套用；保留现有执行环境，先核实 EL、SMC/HVC 和可用内存 |

源码入口：[GCC 驱动列表](https://github.com/u-boot/u-boot/blob/ee9fc27f2a2a43f253a6fe18d7a6f7c4eb0cda3b/drivers/clk/qcom/Makefile)、[UFS PHY](https://github.com/u-boot/u-boot/blob/ee9fc27f2a2a43f253a6fe18d7a6f7c4eb0cda3b/drivers/phy/qcom/phy-qcom-qmp-ufs.c)、[FDT/内存交接](https://github.com/u-boot/u-boot/blob/ee9fc27f2a2a43f253a6fe18d7a6f7c4eb0cda3b/arch/arm/mach-snapdragon/board.c)。

阶段三实施顺序：

1. 先制作仅观测的 U-Boot 负载，解决屏幕/日志输出和接收真实内存信息，保留 Android 默认启动项。
2. 补齐 SM8750 存储所需驱动；暂不做磁盘安装或文件系统写入。
3. 再用临时 Entry 1 验证只读列举存储及分区、内存布局和 EFI。
4. 通过检查点 3 后，才进入 USB 根文件系统和 Armada 内核/DTS 的阶段四。

本轮未刷入最终 boot，未改变 userdata/GPT，未触及 ABL 或其他禁止写入的分区。


## 12. 阶段三观测负载实机通过（尚未验证存储/EFI）

代码提交（均不包含本文、README 或原始会话记录）：

- kShimC `cdb0c8b`：已验证的 PON/短按修复及测试。
- Armada `e729850`：已验证的 OEM ABL 保护及测试。
- Armada `72f21f2`：只操作 RAM 的 U-Boot observer、构建脚本和测试。

观测负载源文件在 `porting/oneplus-pad-2-pro/uboot/`，基于第 11 节的固定 U-Boot 提交。使用独立 Kconfig 开关，原先的高通通用路径保持默认行为；observer 分支直接取原始 ABL FDT 的真实内存信息，生成仅有内存/保留区、PSCI、RAM 日志和 simplefb 的控制树。原始 FDT 保持不变。

- U-Boot 装入 `0xb9000000`，初始栈及负载的 16 MiB 窗口已通过实际 FDT 的可用内存和保留区检查。
- 使用 64 位地址构建 RAM 白名单映射，静态保留区排除在普通映射之外，帧缓冲单独映射。
- UFS、MMC、SCSI、USB、MTD、fastboot 写入、EFI loader、持久化环境等均未启用；构建脚本检查相关配置。
- 用户看到黑底白字文字控制台；实测 EL1，14 个非空 DRAM bank，高地址重定位为 `0xbffead000`；simplefb 为 3392×2400×32，地址 `0xfc800000`。

首次两版通过 PSCI reset 返回 Android：一版旧 RAM 日志有散布的字符损坏，另一版没有留下旧 console 文件。ramoops 实测 ecc=0，说明经固件重启的日志保存不可靠。加入五份副本、CRC 和多数恢复后，也不能保证内核还能找到被重启破坏的主头部，因此它不能替代稳定的交接路径。

第三版改为观测结束后直接交回 **ABL 已加载在 RAM 中的原 WildKernel**：先校验原 FDT CRC 和内核入口头部没有改变，再执行 U-Boot 的 Linux 清理函数关闭缓存/MMU，从 `0xa8000004` 进入原内核第二条指令；x0 仍是原始 ABL FDT，x1–x3 为 0。第三版只适用于没有存储/SoC 驱动操作的 observer；第 13.3 节的后续 NOP 版本增加了队列与链路状态恢复检查，任一步骤不确定时拒绝直接交回 Android。这些都不是让完整 U-Boot 从磁盘重新加载 Android。最终双启动的默认 Android 项仍直接由 kShimC 启动。

第三版实测结果：

- `OPD2413_OBSERVER_READY EL1 original_fdt=b6947000`。
- `OPD2413_OBSERVER_END` 后输出 `OPD2413_ANDROID_HANDOFF fdt_crc=68a182bd entry=a8000004`。
- Android 内核读回 202186 字节的 replicated console，CRC 通过，第一副本无需修复任何字节。
- `sys.boot_completed=1`、WildKernel `6.6.118-android15-OP-WILD`、KernelSU root 正常。
- 用户确认锁屏、指纹、Wi-Fi、KernelSU/susfs 和显示均正常。
- `boot_b` 哈希仍是第 10 节记录的值，全程仅 `fastboot boot`，未写设备分区。

离线验证：ASan/UBSan 覆盖 FDT 不变性、过滤节点、保留区、高地址切分、溢出与错误输入，以及日志初始化、重复探测、容量边界、损坏恢复和 CRC 拒绝。QEMU 分别从 EL1/EL2 运行同一 observer；用 ARM64 检测负载验证原内核交接处的 FDT、清零的 x1–x3、关闭的 MMU/D-cache，并检查 framebuffer 中确实有非单色内容。

电脑上的结果目录：`output/opd2413-stage3/`（Git 忽略）。可用镜像为 `observer3-boot.img`，SHA-256 为 `0c0c0257e6a1ca6c93aca92a573ff7e112629f4af61e4d1b663e2be05098b740`。另有 `observer3-Image`、`unpacked3/`、`hardware-observer3.log`、`observer3-pstore.bin`、`validation.json`、`SHA256SUMS` 和 `qemu-handoff/`。当前镜像仍以 ColorOS 为默认项，30 秒超时；第二项是 U-Boot observer。

**后续 UFS 观测、链路诊断及单次 DMA 结果见第 13 节。当前下一项是只读 LUN/GPT 和 EFI 验证；检查点 3 尚未通过，尚不能进入分区安装或宣称 Armada 已能启动。**


## 13. UFS / SMMU 只读寄存器观测（2026-11-02）

本节记录两轮只读寄存器观测版本，均通过 `fastboot boot` 临时启动；后续 DME_GET 与 NOP 版本见 13.2、13.3。第二轮包含硬件自动门控识别及 SMMU 流/上下文读取。这两个只读版本的所有 MMIO 页都是 Device、只读、不可执行；未启用存储驱动，未发送 UFS 命令，未改变控制器、时钟、PHY 或 SMMU 配置。此结论不包括后续显式启用 UIC/队列操作的 NOP 版本。

- 第一轮 AHB_CBCR 为 `0x88000003`，有 SW enable 和 HWCG，idle CLK_OFF=1。第一版保守跳过了主机寄存器。参照 Linux v7.2 `drivers/clk/qcom/clk-branch.c` 的 HWCG 处理后，第二轮无需改时钟即可正常读取主机。
- UFSHCI `VER=0x400`，高通 `HW_VERSION=0x60010000`（6.1.0）。`CAP=0xd5c7031f`，支持 64 位 DMA；MCQ 可用但 `MEM_CFG=0`，当前为 legacy single-doorbell 路径。
- `HCE=1`，`HCS=0x10f`，`IE=0`，`AHIT=0`。传输队列地址 `0xd6521000`，task 队列地址 `0xd6522000`；两个 doorbell 均为 0，UTRLRSR=1，UTMRLRSR=0。
- PHY 的 `PCS_START=1`，`POWER_DOWN=1`，`SW_RESET=0`，`READY=0x0d`。两个 UFS 电源域为 on。
- 原厂 FDT 的 UFS SID 为 `0x60`，不能照搬主线 SM8750 DTS 的 `0x600`。实际 SMR0=`0x80000060`，S2CR0=0，context 0；`CBAR=0x0001f000`，`CBA2R=1`，`SCTLR=0x0a9f00e0`（M=0），TTBR0/1、TCR、MAIR 均为 0。SMMU stage-1 翻译未开启；这与当前 ABL 的 identity DMA 状态相符，后续单次 DMA 验证见 13.3。
- 原 FDT CRC 为 `cc836a04`，直接返回原 WildKernel 后 `sys.boot_completed=1`，`boot_b` 哈希仍保持原值。用户确认显示、解锁、指纹、Wi-Fi、KernelSU/susfs 全部正常。

验证覆盖 512 种时钟/电源/复位/HWCG 组合，SMMU 精确/掩码匹配、旁路/故障、错误上下文边界、4 KiB/64 KiB 页布局、FDT phandle 拒绝。QEMU EL1/EL2 检查假的 UFS 寄存器状态、无 SMMU 时的保护、日志、显示和 Linux 入口约定；没有模拟真实 UFS 传输。

第二轮结果：`output/opd2413-stage3/ufs-snapshot2/`，包含 `snapshot-boot.img`、`hardware.log`、`hardware-pstore.bin`、`validation.json` 和 `SHA256SUMS`。镜像 SHA-256 为 `11f399777a382d44fd92dff5b59e48a5e8148430a4f09c15123279f12ba8b8a0`。第一轮保留在 `ufs-snapshot/`。

只读观测后已继续完成 DME_GET 和一次 NOP/DMA 测试，见 13.2、13.3；仍未接入 SCSI/块设备、读取 GPT 或启用 EFI。检查点 3 仍未通过。


### 13.1 首版 NOP DMA 实验超时（修正版结果见 13.3）

实验镜像保留在 `output/opd2413-stage3/ufs-nop/`。其可写 MMIO 仅限 UFS 第一页，写函数还限制为 IS、UTRLBA、UTRLBAU、UTRLDBR、UTRLRSR 五个寄存器；GCC、PHY、SMMU 仍只读。固定 DMA 窗口 `0xb9800000+64KiB` 在原 U-Boot 16MiB 装载区内，运行时从可用 RAM 中扣出后单独映射 Normal-NC，无 WB 别名。仅发送 NOP_OUT，不发送 SCSI 或 UFS query 写命令。

该轮返回 Android 后 RAM 日志 CRC 未通过，因此其内容只能作为未验证的诊断线索。损坏副本中的相关文本一致指向 OCS 仍为 0xf、doorbell 为 1、IS 为 0x400，等待超时，保护分支拒绝直接跳转 Android，随后执行固件复位。用户没有看清具体重启过程。主机校验 `sys.boot_completed=1`、b 槽、boot_b 原哈希均正常；用户确认解锁、指纹、Wi-Fi、KernelSU/susfs 仍正常。

首版失败不能作为 DMA 或存储成功的证据；该失败版本未提交。随后先完成了不启动 DMA 的 DME_GET 链路诊断，再验证修正版 NOP，并将通过的代码提交为 `41c50ca`，见 13.2、13.3。


### 13.2 DME_GET 实机确认：ABL 已主动让链路进入 Hibern8

`output/opd2413-stage3/ufs-link/` 中的独立诊断不配置 UTP 队列、不启动 DMA，只发送三个 DME_GET，完成后恢复 UIC 参数及 completion 状态。实测原 UIC_CMD 为 `0x17`（DME_HIBER_ENTER），TX_FSM lane0/lane1 均成功读到 `1`（Hibern8），PA_PWRMODE 为 `0x11`。日志 CRC 完整、无需修复，原 FDT CRC 为 `4f2fe040`；直接交回原 WildKernel 后用户确认 Android 功能正常。这确认原 NOP 实验至少缺少退出休眠的步骤，不能将 HCE=1 当作链路可直接传输的证明。

用户同时报告原厂 START/Recovery/Power off 菜单停留变长。进行未加载任何临时镜像的原始 boot_b 对照：将 adb reboot bootloader、fastboot 槽位检查及正常 reboot 连续执行。主机从发出重启到识别 fastboot 为 13.498 秒，识别后 0.216 秒内完成发送正常重启。用户确认该轮菜单停留明显缩短、已回到 ColorOS。前后 Android 日志均未检出 UFS 传输超时或 SMMU fault，boot_b 原哈希不变。此结果支持等待主要来自电脑侧分步操作；后续临时测试将连续执行连接检查和 boot 命令，减少原厂菜单停留。

在此基础上实现的修正版严格检查原始 Hibern8 状态，执行 Hibern8 exit、单次 NOP、原队列恢复、Hibern8 enter；任意步骤不确定时禁止直接交回 Android，改走固件复位，并增加只读 SMMU fault 寄存器输出。该修正版已经通过第 13.3 节的真机测试；其结论限于单次通信及状态恢复，尚未验证存储读命令。

### 13.3 修正版 NOP/DMA 真机通过（当前检查点）

2026-11-02，临时启动 `UFS NOP test`，实机 EL1 的完整成功路径如下：

```text
UFS_LINK TX_FSM_L0 result=0 value=00000001
UFS_LINK TX_FSM_L1 result=0 value=00000001
UFS_LINK PA_PWRMODE result=0 value=00000011
UFS_H8_EXIT_PASS
UFS_NOP DMA=b9800000 original_queue=00000000d6524000
UFS_NOP OCS=00000000 response=20 tag=00 IS=00000401
UFS_H8_ENTER_PASS
OPD2413_UFS_NOP_PASS original_queue_and_hibern8_restored
OPD2413_ANDROID_HANDOFF fdt_crc=51de2f93 entry=a8000004
CRC verified; 0 bytes recovered relative to first replica
```

- 使用独立的 `0xb9800000+64KiB` RAM DMA 窗口，Normal-NC 映射，无 WB 别名。真实收到了 `NOP_IN` 应答，OCS=0、task tag=0。
- 原队列地址从硬件读取后保存、恢复，本轮为 `0xd6524000`，与前几轮的地址不同；实现没有把它写死。恢复队列后，重新进入原先的 Hibern8 状态，再交回原 WildKernel。
- NOP 版仅为 UFS 第一页 MMIO 开放必要写入，并由写函数进一步限制为 UIC 参数/命令、completion 清除和 UTRL 队列寄存器。UIC 命令仅允许 DME_GET、Hibern8 enter/exit；GCC、PHY、SMMU 配置仍不写入。
- `sys.boot_completed=1`，仍为 b 槽。Android 内核日志未检出 UFS 传输超时或 SMMU fault。用户确认显示、解锁、指纹、Wi-Fi、KernelSU/susfs 全部正常，原厂 bootloader 菜单的等待也已缩短。
- `boot_b` 前后哈希仍为 `0e5afd65d6c92bb12c3a319ddcbcfc9ffaac6a68e22c567c4a6ef1cf82705e49`。NOP 负载未执行存储读写命令；Android 侧只读校验了 `boot_b`。未刷写设备分区，未调整 GPT/分区布局、未格式化 userdata、未刷写 OEM ABL。

**本次证明的范围**：复用 ABL 初始化状态，在当前执行环境与受限地址条件下，完成一次有效 UFS DMA 通信，并恢复队列与休眠状态、正常启动 Android。**尚未证明** LUN 枚举、容量读取、GPT/文件系统读取、EFI、完整 SM8750 驱动重新初始化、Armada 内核启动或双系统长期稳定性。

验证与提交：

| 项目 | 结果 |
|---|---|
| 主机 ASan/UBSan | FDT/保留区/日志、512 种时钟状态、SMMU 边界、NOP 布局、DMA 边界保护、队列与 UIC 状态恢复均通过 |
| 故障路径 | 覆盖超时、OCS/UPIU 错误、错误 task tag、队列恢复失败、UIC 拒绝和 Hibern8 超时；不确定状态禁止直接交回 Android |
| QEMU EL1/EL2 | 同一负载的假硬件保护、CRC 日志、实际帧缓冲内容和 Linux 入口约定通过；不模拟真实 UFS 传输 |
| 默认 observer 回归 | 不启用探测功能的构建与 QEMU 交接检查通过；overlay 重复应用不产生重复对象 |
| Armada `1477f95` | 已验证的 UFS/GCC/PHY/SMMU 只读寄存器观测代码 |
| Armada `41c50ca` | 已验证的 DME_GET、Hibern8 唤醒/恢复、单次 NOP/DMA 和对应保护/测试 |

两个提交均只含代码与测试，不含本文、README 或原始会话记录。最终通过的镜像与证据位于 `output/opd2413-stage3/ufs-nop2/`（Git 忽略）：

| 文件 | 内容 |
|---|---|
| `nop-boot.img` | 已实机通过的临时 boot 镜像，默认仍为 ColorOS，30 秒菜单超时 |
| `hardware.log`、`hardware-pstore.bin` | 通过 CRC 校验的解码日志与原始副本 |
| `validation.json`、`SHA256SUMS` | 测试结果与产物哈希 |
| `android-after-nop-dmesg.txt` | NOP 后 Android 内核日志 |
| `launch-timing.json` | 连续执行连接检查与临时启动的计时记录 |

`nop-boot.img` SHA-256：`ad0035e36259218046d31b958c56e1d239babf17efe189efe3c54eca45f12663`。

**接续顺序**：只读 UFS LUN/容量 → GPT 读取与备份对照 → EFI 程序与加载路径 → 通过检查点 3 → Armada 内核/设备树与 USB 外置根文件系统试启动 → 设备功能适配 → 安装和固化双启动。下一轮仍不进行分区或文件系统写入。


## 14. UFS 只读 LUN/容量/GPT 读取（2026-11-02，检查点 3 的存储部分）

### 14.1 实机过程

全部使用 `fastboot boot` 临时启动；菜单默认仍为 ColorOS，30 秒超时。

| 轮次 | 镜像目录（`output/opd2413-stage3/`） | 结果 |
|---|---|---|
| read1 | `ufs-read/` | U-Boot 运行后固件复位，RAM 日志丢失。ABL 日志 `HARD_RESET`、`Fastboot=0` 证明不是直接交接 |
| read2（`--halt-on-failure`） | `ufs-read2/` | 用户拍照：发往 W-LUN 0x81 的 `REPORT LUNS` 传输正常（OCS=0、RESPONSE UPIU、tag 正确），但 response=0x01（Target Failure）。旧代码把它当成传输失败，拒绝交接。SMMU `CB0_FSR=0x400` 只是 FORMAT 字段，不是故障 |
| read3 | `ufs-read3/` | Target Failure 改为“命令已完成”处理后，流程完整走完并直接交接。sense 显示 LUN 0–5 和 W-LUN 均为 **2/04/02 NOT READY, INITIALIZING COMMAND REQUIRED**，LUN 6–7 为 5/25/00（不存在） |
| query（方案 A，只读） | `ufs-query/` | `bCurrentPowerMode=0x33`（**UFS-PowerDown**），`bBootLunEn=0x02`，`fDeviceInit=0`，`bNumberLU=6`，`bInitPowerMode=0x01`，UFS 4.0，厂商 0x01CE（Samsung，Android 显示 KLUFG4NHHB-F0G1） |
| wake（方案 B） | `ufs-wake/` | 唤醒到 Active（0x11）→ 6 个 LUN 容量、主备 GPT 均有效 → 恢复 PowerDown 并查询确认为 0x33 → Hibern8 → 直接交接。**66/66 个 GPT 块与阶段一备份 CRC 一致**，列出 133 个分区 |

结论：ABL 在交接前先用 START STOP UNIT 让 UFS 设备进入 PowerDown，再让链路进入 Hibern8。Linux 启动时会重新初始化 UFS，所以不受影响；U-Boot 要读存储，就必须先唤醒设备，读完再按原状态恢复。

### 14.2 代码（armada `porting/oneplus-pad-2-pro/uboot/`）

- `ufs_read.c/h`、`ufs_read_test.c`：`--ufs-read`（`CONFIG_OPD2413_UFS_READ`）只发 STANDARD READ query（READ DESCRIPTOR/ATTRIBUTE/FLAG）和 `TEST UNIT READY`、`REPORT LUNS`、`READ CAPACITY(10/16)`、`READ(10)`（只读 LBA 0–5 和最后 5 块），提交前逐条校验白名单。`--ufs-wake`（`CONFIG_OPD2413_UFS_WAKE`，隐含 `--ufs-read`）另外允许向 0xD0 发 `START STOP UNIT`，只限 POWER CONDITION 1/2/3、IMMED=0，并且只在读到 0x22/0x33 时使用；恢复后必须查询确认原值，否则拒绝交接。
- `ufs_nop.c/h`：前置检查抽成共享函数 `opd2413_ufs_preflight()`，NOP 行为不变。
- `--halt-on-failure`：交接被拒绝时停在屏幕上，不执行会冲掉 RAM 日志的固件复位（仅用于调试）。
- 传输层错误（OCS、UPIU 类型、tag、剩余字节、超时、IS 错误）仍然拒绝交接。Target Failure 和 CHECK CONDITION 属于已完成的命令。
- 验证：
  - 宿主机 ASan/UBSan 模拟器独立检查每个 UTRD/UPIU/PRDT 字段，覆盖 3 个合成 LUN、UA 重试、Target Failure、PowerDown 设备、唤醒、唤醒被拒、恢复不一致、每条命令位置上的 4 类传输故障、异常状态时零写入；
  - 共 19 项变异测试全部被抓到（另有 1 项与 `scan_part` 重复的检查刻意保留）；
  - QEMU EL1/EL2：读取版、唤醒版、默认 observer、NOP 版全部通过。
- 已提交 `dc9c71a`（只读读取 + Target Failure 处理）。query、唤醒、`--halt-on-failure` 改进尚未提交。

### 14.3 安全与设备状态

- 每轮之后 `boot_b` SHA-256 都仍为 `0e5afd65d6c92bb12c3a319ddcbcfc9ffaac6a68e22c567c4a6ef1cf82705e49`，Android dmesg 中 UFS 超时/中止和 SMMU fault 计数为 0。
- 发出的命令中没有任何介质写入；唯一改变状态的是 0xD0 上的电源模式切换，并已恢复为 ABL 的原值。
- read1、read2 的强制或固件复位后，ColorOS 均正常启动。

### 14.4 下一步

1. 只读读取 ESP 所在分区。本机没有 Armada ESP，可先读一个已知 FAT/ext4 分区的超级块，验证多块 READ(10) 和更大的 LBA 范围。
2. 设计 EFI 验证路径：U-Boot 需要块设备层（BLK/partition）才能从 GPT 分区加载 EFI 程序。可以选择把本文的最小读路径封装成 U-Boot 只读块设备驱动，或者移植完整的 SM8750 UFS 驱动。两条路线待评估。
3. 可选小改进：`REPORT LUNS` 返回的长度小于分配长度属于正常情况（本次返回 56 字节，剩余 4040），可以接受并解析，而不是回退到逐个探测。

## 15. 只读 U-Boot 块设备与文件系统读取（2026-11-03，提交 `b2b3867`）

### 15.1 结果

镜像 `output/opd2413-stage3/ufs-blk5/read-boot.img`（`--ufs-blk --ufs-trace --halt-on-failure`），菜单项 "UFS block FS test 5"。U-Boot 把 6 个 LUN 注册为只读块设备 `opdufs 0`–`opdufs 5`，块大小 4 KiB，然后在命令行阶段读取文件：

| 分区 | 文件 | 大小 | 用时 | CRC32（与 Android 一致） |
|---|---|---|---|---|
| modem_b（`opdufs 4:2a`，FAT16） | `/image/adsp.b00` | 1812 | 97 ms | `091f5903` |
| modem_b | `/image/adsp.b09` | 3117680 | 503 ms | `33d872c3` |
| modem_b | `/image/adsp.b38` | 7612348 | 959 ms | `4e32fefe` |
| dsp_b（`opdufs 4:2d`，ext4） | `/adsp/AudioSphereModule.so.1` | 82912 | 36 ms | `71d2f760` |

`ext4ls` 列出 77 个文件、2 个目录。之后恢复 `bCurrentPowerMode=0x33`、ABL 队列和 Hibern8，直接返回 Android（ABL 日志 `Fastboot=1`）。`boot_b` SHA-256 不变，dmesg 无 UFS/SMMU 错误，Wi-Fi 正常。

### 15.2 调试过程中发现的问题

1. **U-Boot 分区号是十六进制**：`4:42` 实际是第 0x42=66 号分区（cpucp_dtb_b），modem_b 应写 `4:2a`，dsp_b 应写 `4:2d`。`fatinfo` 不支持 `#分区名`。
2. **悬空指针（根因）**：session 保存的是 observer 中 board-init 栈帧里 `io` 结构体的地址。main_loop 复用这段栈后，命令行阶段的每条 UFS 命令都会经由残留的返回地址跳回 board_init_r，表现为无打印地重新执行 bootcmd、无限滚屏，最后 hush `xrealloc failed`。现在 session 在 open 时复制 io，并有主机回归测试。
3. `crc32` 命令依赖 `CMD_MEMORY`（附带 mw/cp），改用只读的 `hash crc32`（`CMD_HASH`）。
4. ramconsole 每份副本只有 48 KiB，长列表（`part list`）只写 RAM 日志或不运行。
5. 定位工具：U-Boot sandbox（`host bind 0 img 4096`）、QEMU 上的 `--ufs-blk-ramdisk` RAM 盘后端（`make_ramdisk.py`），以及 `--ufs-trace`（门铃前后栈校验、main_loop 进入报告）。

### 15.3 下一步

1. 用不带 trace 的最终 `--ufs-blk` 构建在实机上回归一次（只在 QEMU 验证过）。
2. EFI：启用 EFI_LOADER，用 LMB/EFI 内存保留保护 ABL 内核、ramdisk、FDT 和 observer 窗口，关闭 EFI 变量持久化，先做 `bootefi hello`。
3. 可选：多命令队列或更大的 PRDT，提高读取速度（当前约 7.6 MiB/s）。

## 16. EFI loader 与 `bootefi hello`（2026-11-03）

镜像 `output/opd2413-stage3/efi1/read-boot.img`（`--efi-hello --halt-on-failure`，隐含 `--ufs-blk`），菜单项 "EFI hello test"。

- 配置：EFI 变量仅在内存（`EFI_VARIABLE_NO_STORE`），关闭 capsule、文件/SPI 变量存储、`setenv -e`、eficonfig、HTTP 启动、TCG2、设置时间；build guard 拒绝这些能力。`EFI_LOAD_FILE2_INITRD` 保留（关闭会导致上游链接错误，且无写路径）。
- 内存保护：late init 开头、块设备注册 EFI disk 之前，从 ABL FDT 读出 FDT 与 initrd（本次 `b6b48000-b7ffe964`），确认都在 `0xa8000000` 所在的同一内存 bank 后，在 LMB 中保留 `a8000000-b7ffe964`、U-Boot 加载槽 `b9000000-ba000000`（含 DMA 窗口）、framebuffer 与 ramoops，并以试分配自检。EFI 内存表中它们都是 `BootServicesData`。同时对 ABL 范围做 CRC，`opd_android` 交接前重新计算，不一致即拒绝返回 Android。
- 实机结果：4 个文件 CRC 正确；`efidebug memmap` 正常；`bootefi hello` 打印 `Hello, world!`，rc=0；ABL 范围 CRC 未变；UFS 恢复后直接返回 Android（`Fastboot=1`）。`boot_b` SHA-256 不变，dmesg 无 UFS/SMMU 错误，Wi-Fi 正常。

### 16.1 systemd-boot（2026-11-03）

镜像 `output/opd2413-stage3/esp1/read-boot.img`（`--efi-esp systemd-bootaa64.efi --halt-on-failure`），菜单项 "systemd-boot test"。systemd-boot 取自 Fedora 44 的 `systemd-boot-unsigned-259.10-1.fc44.aarch64`（签名已校验），与 Armada 镜像所用一致。

- build.py 用 mtools 生成 4 MiB FAT ESP（`/EFI/BOOT/BOOTAA64.EFI`、`/loader/loader.conf`，无启动项），以 `.incbin` 嵌入 U-Boot 只读数据；U-Boot 用 `blkmap` 把它映射为块设备，`load` 后 `bootefi <addr>`（不能带 `:size`，否则会丢掉 load 记录的设备路径，systemd-boot 就看不到 ESP）。
- 实机结果：U-Boot 从 blkmap 读出的 systemd-boot CRC32 `cb7ef051` 与原文件一致；systemd-boot 启动、读取 ESP，报 `No loader found. Configuration files in \loader\entries\*.conf are needed.` 并返回（`r = 14`，即 EFI_NOT_FOUND，符合预期）；ABL 范围 CRC 未变，UFS 恢复后直接返回 Android（`Fastboot=1`）。
- 新增构建检查：U-Boot 镜像以及预重定位初始栈（`__bss_end + SYS_INIT_SP_BSS_OFFSET`）不得与 `0xb9800000` 的 DMA 窗口重叠。嵌入 ESP 后镜像为 4.75 MB，初始栈在 `0xb98863d8`，仍不重叠。

### 16.2 按键输入与 systemd-boot 菜单（2026-11-03，提交 `05741e8`）

镜像 `output/opd2413-stage3/menu2/read-boot.img`（`--efi-esp systemd-bootaa64.efi --efi-menu --halt-on-failure`），菜单项 "systemd-boot menu test 2"。

- 按键（`keys.c`）：通过 SPMI 仲裁器 v7 的 EE0 observer 通道只读读取。电源 = PMK8550（SID 0）PON `0x1310` bit 7，音量上 = 同一寄存器 bit 6（resin），音量下 = PM8550（SID 1）GPIO6 `0x8d10` bit 0（低有效）。唯一的写操作是向 observer 命令寄存器写 EXT_READL 单字节命令，其余写入 panic。实机 APID：PON 581、GPIO6 610。去抖；启动时已按下的键不算新按键。仅在 EL1、stream 0x60 且 CPU implementer 为 Qualcomm（0x51）时启用。
- 作为 U-Boot 输入设备 `opdkeys`：音量上 = `ESC [ A`，音量下 = `ESC [ B`，电源 = 回车；`opd_keys` 步骤三键都确认后才切换 stdin 并启动菜单。
- ESP 中有 "EFI hello" 条目（U-Boot helloworld.efi）和 systemd-boot 的 "Reboot The System"（默认，30 秒）。EFI ResetSystem 先尝试 `opd_android` 的带校验返回，被拒绝才真正复位。EFI 构建开启 `VIDEO_ANSI`（否则菜单显示为原始转义序列）。
- 实机结果：三键识别正常；菜单倒计时在按键后停止，音量键移动高亮，电源键运行 EFI hello 并返回菜单（两次），选择 Reboot 后直接返回 Android（`Fastboot=1`）。`boot_b` 不变，dmesg 无 UFS/SMMU 错误，Wi-Fi 正常。

### 16.3 下一步

1. 启动 Linux：在 ESP 中加入 Type #1 启动项，用 EFI stub 启动内核。需要 Armada 的 SM8750 内核与设备树（阶段四）；可先选一个已知能在 SM8750 上启动的主线内核做最小验证（只到串口/屏幕早期输出）。
2. ESP 的最终位置（USB 存储或 UFS 上新分区）仍待决定；在此之前继续不写 UFS。

## 17. 阶段四：通过 EFI 启动主线 Linux（2026-11-03）

内核取自 yzddmr6 的 erhai 移植（`github.com/yzddmr6/linux-sm8750-erhai`，分支 `erhai/v0.6`，commit `70b5725`，基于 v7.2.14，板级 DT `sm8750-erhai.dts`），配置 = arm64 defconfig + `erhai-firstboot.config` + `EFI_ZBOOT`/Terminus 16×32 字体。参考集成仓库 `../onepluspad-2pro-mainline`（其安装/切换方式需重新分区和改写启动分区，本项目不采用）。

- 启动链：kShimC →（FreeExec）U-Boot → systemd-boot →（EFI stub，`vmlinuz.efi`）erhai 内核。ESP（20 MiB FAT：systemd-boot、内核、DTB、initramfs、启动项）作为 kShimC manifest 中名为 `OPD2413 ESP` 的 `Type=Blob` 条目随打包镜像由 ABL 载入（不可执行、不进菜单），U-Boot 解析 manifest 后用 blkmap 映射。ExitBootServices 时 U-Boot 把 UFS 恢复为 ABL 交接状态；ramoops 区在 EFI 内存表中标为保留。
- 冒烟 DT（`porting/oneplus-pad-2-pro/linux/smoke_dtb.py`）：关闭 UFS 与 UFS PHY；ramoops 移到 Android 的区域与布局（`0x880000000`，console 0x40000、pmsg 0x200000、ecc 0），PSCI 复位后 Android 的 `/sys/fs/pstore/console-ramoops-0` 即为 Linux 控制台日志（PMIC 强制重启会丢失）。
- initramfs（`linux/smoke_initramfs.py`，固定版本的 Ubuntu busybox-static）只挂 proc/sys/devtmpfs，打印诊断后 120 秒 `reboot -f`。
- 结果：Linux 1.3 秒进入用户态，8 核在线，约 14.5 GiB 内存，EFI 运行时可用。**msm DRM（MDSS 开启）接管 ABL 点亮的面板并正常显示**（card0：DSI-1/DP-1/Writeback-1，fbcon 大字体），NT36536 触控识别芯片（无固件）。关闭 MDSS 只用 efifb 时，fbcon 画出后很快黑屏：led-backlight 先置 `bl_power=4`（已在 init 中打开），关闭 KTZ8868 与 SMMU bypass 仍黑屏，原因未查明；这条诊断路线已放弃。
- 每轮 `boot_b` SHA-256 不变，PSCI 复位后正常回到 ColorOS。

### 17.1 本阶段提交与遗留问题

- 提交（armada `main`）：`fd7c567` porting: boot mainline Linux on OPD2413 through U-Boot, systemd-boot and EFI。
- 遗留问题：
  1. 关闭 MDSS、只用 efifb 时 fbcon 画出后很快黑屏（已排除 led-backlight 的 `bl_power`、KTZ8868 偏压芯片、SMMU bypass），原因未查明。msm DRM 可用，此路线已放弃。
  2. 4B（msm DRM）那一轮 Android 的 pstore 为空，原因未查；之前 MDSS 关闭的几轮能读到 Linux 日志。
  3. Armada 7.2.6 内核在本机的构建被中途停止（`/var/tmp/armada-kernel-build`，293 个补丁已打上，安装 pahole 后可用 `FAST=1` 继续）。
  4. 此前 erhai 本地只有 1 个提交；现已补足到所需 v7.2.14 基线，仍是浅克隆，后续进度见第 19 节。

### 17.2 下一步

1. 取得 erhai 相对 v7.2.14 的补丁（补历史，或下载 v7.2.14 源码做差异），分出：板级 DT、OPD2413 专用驱动（面板、NT36536、KTZ8868、pmic-glink 变体、pogo 键盘等）、SM8750 通用修复（与 Armada 现有补丁重叠的部分）。
2. 移植进 Armada 补丁栈（7.2.6）：`packages/kernel/dts/` 增加 OPD2413 板级 DT，`patches/` 增加驱动补丁并在 `PATCHES.md` 记录来源；构建后先用同样的冒烟方式（UFS 关闭）实机验证。
3. 只读存储验证：冒烟内核打开 UFS，只做识别或只读挂载。
4. 决定根文件系统与 ESP 的位置（USB 存储，或 UFS 新分区——写入须用户单独批准）。

## 18. 当前状态快照与复现指南（2026-11-03）

### 18.1 已提交的代码（armada `main`，`porting/oneplus-pad-2-pro/`）

| 提交 | 内容 |
|---|---|
| `dc9c71a`、`a70deb4` | 只读 UFS LUN/容量/GPT 调查；唤醒与恢复 |
| `b2b3867` | 只读 `opdufs` 块设备（session 保存 io 副本，修复悬空指针） |
| `847e8fd` | EFI loader；ABL 交接内存保留与 CRC 校验 |
| `130ab6a` | 嵌入式 ESP + systemd-boot |
| `05741e8` | 只读 SPMI 按键输入；systemd-boot 菜单；EFI 复位返回 Android；VIDEO_ANSI |
| `fd7c567` | ESP 作为 kShimC Blob；ExitBootServices 恢复 UFS；EFI 保留 ramoops；多 Linux 启动项；`linux/smoke_dtb.py`、`linux/smoke_initramfs.py` |

### 18.2 外部源码与工具位置

| 内容 | 位置 |
|---|---|
| U-Boot 源码（固定版本，`prepare.py` 打补丁） | `/tmp/opd2413-debug/u-boot`（/tmp，重启后需重新准备） |
| U-Boot sandbox（FS 复现） | `/tmp/opd2413-sandbox` |
| kShimC 打包工具 | `~/github/kShimC/build/manifest-regression/packer/{MultiBootKernelPatcher,UnpackTool}`；源码 `~/github/MultiBootKernelPatcher` |
| boot 镜像重打包 | `~/oneplus-pad2pro-work/scripts/repack_boot_v4.py`，模板 `~/oneplus-pad2pro-backup/slot_b/boot_b.img` |
| ColorOS 基础内核 / shim | `~/oneplus-pad2pro-work/stage2j/wildkernel-Image`、`~/oneplus-pad2pro-work/stage2k/t14-shim.bin` |
| ABL 运行时 FDT（构建/QEMU 用） | `~/oneplus-pad2pro-backup/fdt/runtime.dtb` |
| systemd-boot 259.10 aarch64 | `/tmp/opd-sdboot-x/usr/lib/systemd/boot/efi/systemd-bootaa64.efi`（Fedora `systemd-boot-unsigned`，签名已校验；/tmp） |
| busybox（initramfs） | `/tmp/opd-busybox-x/usr/bin/busybox`（Ubuntu busybox-static 1.36.1-6ubuntu3.1 arm64，sha256 已对 erhai `source.json` 核对；/tmp） |
| erhai 内核源码 | `~/github/linux-sm8750-erhai`（`erhai/v0.6`，`70b5725`，浅克隆） |
| erhai 集成仓库 | `~/github/onepluspad-2pro-mainline` |
| erhai 内核构建产物 | `output/erhai-kernel/build`（`vmlinuz.efi`、`sm8750-erhai.dtb`），配置片段 `output/erhai-kernel/opd-smoke.config`（EFI_ZBOOT、FB_EFI、FONT_TER16x32 等） |
| 最新可用实机镜像 | `output/opd2413-stage4/linux5/read-boot.img`（kShimC "Linux smoke test 5"；systemd-boot 中 4B = msm 显示，4C = 诊断） |

### 18.3 构建与打包（摘要）

1. erhai 内核：`make -C ~/github/linux-sm8750-erhai O=output/erhai-kernel/build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig`，`scripts/kconfig/merge_config.sh` 合并 `erhai-firstboot.config` 与 `opd-smoke.config`，`olddefconfig`，再 `-j15 vmlinuz.efi qcom/sm8750-erhai.dtb`。
2. DT：`python3 linux/smoke_dtb.py --input .../sm8750-erhai.dtb --out erhai-mdss.dtb`（`--no-mdss` 为诊断版）。
3. initramfs：`python3 linux/smoke_initramfs.py --busybox <busybox> --out initramfs.cpio.gz`。
4. U-Boot + ESP：`python3 uboot/build.py --source /tmp/opd2413-debug/u-boot --out <dir> --fdt <runtime.dtb> --ufs-blk --efi-esp <systemd-bootaa64.efi> --efi-menu --esp-blob --linux vmlinuz.efi --linux-dtb <dtb> --linux-initrd initramfs.cpio.gz --linux-options "console=tty0 earlycon=efifb fbcon=font:TER16x32 clk_ignore_unused pd_ignore_unused regulator_ignore_unused rdinit=/init panic=30 ignore_loglevel" [--linux-entry "标题|dtb|参数"] --halt-on-failure --jobs 15`。
5. QEMU 回归：`python3 uboot/qemu_observer.py --binary <dir>/build/u-boot-nodtb.bin --fdt <runtime.dtb> --out <dir>/qemu --handoff --ufs-snapshot on --smmu-snapshot --ufs-read --manifest-blob <dir>/esp.img`（RAM 盘变体另加 `--ufs-blk-ramdisk` 构建与 `--blk-ramdisk --loader ...@0x900000000`）。
6. 打包：`read.cfg` 含 `[Image-A]` ColorOS（BaseImage）、`[Image-B]` U-Boot（FreeExec，CopyTo 0xb9000000）、`[Image-C]` `Name=OPD2413 ESP`、`Type=Blob`、`Align=12`；`MultiBootKernelPatcher read.cfg`，`UnpackTool` 逐项 `cmp` 核对后 `repack_boot_v4.py` 生成 `read-boot.img`。
7. 实机：`adb reboot bootloader` → `fastboot boot read-boot.img`（只临时启动）。

### 18.4 调试经验

- U-Boot 分区号是十六进制（modem_b = `4:2a`）；`crc32` 需 CMD_MEMORY，改用 `hash crc32`；ramconsole 每份只有约 48 KiB。
- 板级 late init 之后仍要用的数据不能指向它的栈帧（悬空 io 指针曾导致 bootcmd 无提示重跑）。
- QEMU 与实机共用 ABL FDT，基于 FDT 的判断（如 UFS stream 0x60）挡不住 QEMU；访问 SoC MMIO 前检查 MIDR implementer 0x51。
- `bootefi <addr>:<size>` 会丢掉 load 记录的设备路径；EFI 程序需要 `VIDEO_ANSI`。
- Linux 日志：DT 的 ramoops 指到 Android 的区域与布局，PSCI 复位后 Android 的 pstore 可读；PMIC 强制重启会丢失，测试时请等自动重启。
- 关闭 MDSS 时 `led-backlight` 会把背光置为关闭（`bl_power=4`）。

### 18.5 安全规则（持续有效）

不刷 `abl_a/abl_b`（OEM 熔断，OpenABL 会变砖，不运行 `flash_abl.sh`）；不重新上锁；不写 `persist`、`nonmodemst*`、`modem`、`xbl*`、`abl*`；不改 vbmeta、分区、GPT、文件系统，不格式化 userdata；不切换到 a 槽；`boot_b` 只写入用户确认过的 kShimC 整分区镜像（2026-11-04 用户批准，必须保留 AVB vbmeta 与 footer，见 §19.39），其他启动分区仍只用 `fastboot boot`；sudo 改用 `pkexec`；构建保留一个 CPU 核（`-j15`）；每次上机前由用户确认，结束后核对 `boot_b` 哈希与 Android 功能；不提交设备标识、本文档、`packages/openabl-updater/README.md` 与对话记录。

### 18.6 Armada 内核移植的起点

- Armada 内核：`packages/kernel`，`BASE.env` 为 7.2.6，`patches/series` 294 个补丁（来源记在 `PATCHES.md`），板级 DT 在 `packages/kernel/dts/`（已有 SM8750 的 `sm8750-konkr-pf-elite.dts`、`cq8725s-ayn-odin3.dts`），构建脚本 `scripts/build-kernel.sh`（`OMP_NUM_THREADS=15` 保留一核；需要 `pahole`，已安装）。
- erhai：`sm8750-erhai.dts`（2340 行，含 pm8550/pm8550ve/pmih0108/pmk8550 与 sm8750-pmics）、驱动如 `drivers/input/touchscreen/nt36536/`、KTZ8868 背光、OnePlus pmic-glink、pogo 键盘；配置片段 `arch/arm64/configs/erhai-firstboot.config`。
- 需比较 erhai 与 Armada 两套 SM8750 补丁的重叠（GPU、DSI、UFS compatible 等），以及 7.2.6 与 7.2.14 的差异。


## 19. Armada 7.2.6 初始移植（2026-11-03，进行中）

- 已在固定 erhai 提交 `70b572544d504735e2924f6c3f5fe3bd726ef275` 附近补历史，确认 v7.2.14 基线为 `e09610b541f2725a6382137c168e4efe208917c3` 且是其祖先。差异共 187 个文件；逐文件试套到 Armada 已有补丁后的 7.2.6，145 个可直接应用、16 个已包含、26 个有重叠。不能整套覆盖已有驱动。
- 当前先引入显示/输入和板级资源：`1400-opd2413-display-panel-and-backlight.patch`、`1401-opd2413-nt36536-and-pogo-input.patch`、`1402-opd2413-sm8750-board-resources.patch`；共选择 43 个文件。完整充电/相机/音频/GPU 驱动扩展未移植，不把设备树中有节点视为这些功能可用。
- 新增 `packages/kernel/dts/sm8750-oneplus-pad2pro.dts`，内容与固定 erhai DTS 逐字节一致。合并共用 `sm8750.dtsi` 时保留 Armada 的 GPU 节点、CPU capacity 和 UART15 pin state，消除重复节点/属性；保留 Armada 的显示补丁。Kconfig 补上 NT36536、KTZ8868、早期供应者和可选 pogo 键盘；EFI/font 冒烟选项单列在 `porting/oneplus-pad-2-pro/linux/armada-smoke.config`。
- 已通过：OPD2413 DTB 编译、29/29 份机型 DTB 编译、在干净 7.2.6 源码上按 series 应用 296 个补丁（无 fuzz 且逐个核对 hunk 数），并确认新补丁涉及的 43 个文件与候选源码逐字节一致。完整 Armada 配置加冒烟选项的 `vmlinuz.efi` 已编译成功，pogo 键盘模块单独编译通过。
- 候选内核在 `output/opd2413-stage4/armada-port/source/`，使用正常 Armada 配置加 OPD/EFI 冒烟选项，`-j12` 编译 `vmlinuz.efi`。保留原 `/var/tmp/armada-kernel-build` 与已通过的 erhai 内核作为对照。
- 证据在 `output/opd2413-stage4/armada-port/`：`erhai-diff-audit.json`、`erhai-versus-v7.2.14.patch`、`selected-groups.json`、`dtb-regression.json`、`clean-series.json`、`build.log`。构建脚本同时改为保留一核，FAST 重跑逐个补登记新 DTB。
- 显示/触控和日志恢复代码已提交为 `043c691`，仅含已验证代码；文档、原始日志与私有固件未提交。后续上机继续复用 RAM/Blob/EFI 链路与冒烟 initramfs，关闭 UFS，不写分区。

### 19.1 Armada 首轮实机验证

- 镜像目录：`output/opd2413-stage4/armada1/`。`read-boot.img` SHA-256 为 `57b611795f88b46a1bc2b3d05690926633d81ede966ecde81be274997d827e35`；内核 EFI 二进制 SHA-256 为 `83dc124a0d1895efe5d434b498650b2057a2c0c67fb127f24097ab944fde25c0`。打包后四个负载均解包逐字节核对；U-Boot EL1/EL2 QEMU 交接回归通过。
- 用户照片（本地保存，未提交）确认 Linux 7.2.6 日志、约 2.6 秒进入 RAM 用户态、msm DRM 设备和自动重启倒计时。约两分钟后自动回到 ColorOS，b 槽及 `boot_b` 哈希未变；用户确认解锁、指纹、Wi-Fi、KernelSU/susfs 全部正常。
- 触控芯片已识别，首次未带 `novatek/nt36536-xn242.bin`，固件请求为 `-2`，因此首轮不能宣称触控可用。USB/SCMI 等存在延迟探测，GPU 固件缺失；相机、充电、音频等尚未完成移植或验证。
- PSCI 复位后的原始 ramoops 文本有随机位损坏，只作为辅助线索。第二轮加入五份状态副本与 SHA-256 校验，只接受完整校验通过的恢复结果；恢复器通过独立位损坏、截断和不可恢复损坏等 6 项检查，实际 ARM64 BusyBox 与 Armada 内核在 QEMU 中也已运行到校验通过的状态输出。

### 19.2 触控固件与第二轮 RAM 测试（通过）

- 从本机备份 `runtime.dtb` 的 `nt36536@0/firmware-data-0` 提取 258048 字节固件，SHA-256 为 `3e3596895975b28459b1937ea35c819b96386b82cbdcc9c61592af8488db702b`，与参考固件清单一致。仅放入忽略的输出目录和 RAM initramfs，未写入设备分区。
- `armada2/read-boot.img` 大小 74235904 字节，SHA-256 为 `a37d26d8a54db6e173d00bd43c10cbf318105202f443ea3e639c079ba44911d2`。保留首轮已验证内核和关闭 UFS 的 DT；加入触控事件读取及初始、20 秒、重启前的状态副本。U-Boot EL1/EL2 QEMU 和四份负载解包比对通过。
- 用户确认进入 Linux 倒计时并完成点按、滑动，自动返回 ColorOS 后全部 Android 检查正常，b 槽及 `boot_b` 哈希未变。
- 解析器已修复标记/换行位损坏和 pmsg 环形边界处理；对剩余单个 Base64 字符错误，仅尝试原始副本中实际存在的候选，并始终以完整 SHA-256 决定接受。8 项恢复/拒绝检查通过。本轮 `initial` 和 `final` 状态完整校验通过；`settled` 未通过，不作为证据。
- 最终状态 SHA-256 为 `6eac62b981fc4561bf158c0e37ad0ab458d45d423620fd8938eeae6e4b2edfb5`，确认 Linux 7.2.6、CPU `0-7`、内存 `15190356 kB`、`msmdrmfb`、DSI-1 和 renderD128；块设备只有 loop0–7，没有 UFS。NT36536 固件加载成功，版本 `0x1D`，共记录 68664 字节（2861 条）触控事件，样本包含 ABS_MT_POSITION_X/Y 和 BTN_TOUCH 按下。手写笔设备已枚举，但未验证笔输入。
- 证据位于 `armada2/verified/final.txt`、`hardware-validation.json` 和 `android-return.json`。这是 Armada 内核加 BusyBox RAM 用户态，尚未启动完整 Armada 系统。

### 19.3 ADSP/SOCCP 固件（第三轮通过，USB 仍阻塞）

- 从本机当前 OTA 的 `~/oneplus-pad2pro-work/stock/modem.img` 只读提取 59 个 ADSP/SOCCP 文件，共 40573096 字节；全部与当前 ColorOS 的 `/vendor/firmware_mnt/image/` 哈希匹配。清单在 `armada-port/firmware/manifest.json`，不使用参考项目旧版固件的哈希代替本机验证。
- 当前 ADSP ELF 的 53 个加载段链接地址跨度为 `0x5080000`，ADSP DTB 跨度为 `0x26e34`；均设置可重定位标志，按 MDT loader 的重定位规则可放入板级 DTS 保留窗口（不能直接把 ELF 链接地址当作最终物理地址）。下一轮仍使用已验证内核与关闭 UFS 的 DT，将固件加入 RAM initramfs；增加 USB/Type-C/remoteproc 状态及延迟探测诊断，状态副本增至 25 份。
- Gzip 固件包超过原 boot 镜像容量限制，改用已启用 `CONFIG_RD_ZSTD=y` 的 Zstd initramfs（23931580 字节），解压逐字节比对及实际内核 QEMU 用户态运行通过。`armada3/read-boot.img` 为 99401728 字节（94.8 MiB），SHA-256 为 `24d011de5764cd69cc17a93a785dc25c90b68d032503b6b7b2760b5b2469fbc4`。四份负载解包比对、RAM ESP initramfs 比对、EL1/EL2 交接回归通过。
- 实机三份状态 `initial/settled/final` 均完整校验通过。最终状态 SHA-256 为 `50f29feab9b1854093b3aacc80fc1707215acaa953904015c7f7dd36cf83a0aa`；SOCCP、ADSP 均为 `running`，NT36536 固件加载正常。USB 尚未注册 UDC，UCSI 初始化失败，SCMI 与 PCIe 仍延迟探测。自动返回后 b 槽及 `boot_b` 哈希未变，用户确认 Android 功能全部正常。

### 19.4 早期供应者与 USB 实时日志（第四轮通过，最新检查点）

- 第三轮初始状态明确记录 `a600000.usb: supplier 88e3000.phy not ready`。DTS 的该节点是 M31 eUSB2 PHY，而原 Armada 配置为 `CONFIG_PHY_QCOM_M31_EUSB=m`；CPUCP mailbox 同样为模块 `CONFIG_QCOM_CPUCP_MBOX=m`。极简 initramfs 不带这些模块。已将两个早期供应者改为内建，未引入新的 PHY/USB 驱动补丁。
- 仅在冒烟配置中内建 configfs ACM 功能，并增加可选 `--usb-log`。该 RAM gadget 只输出 `/dev/kmsg`，没有交互 shell、不配置存储功能；普通 Armada 配置保留 gadget 模块化。新增主机接收脚本 `linux/capture_usb.py`，仅匹配专用 VID/PID/序列字符串；伪终端断开测试通过。
- 新内核 SHA-256 为 `17096f22c18552ca47e1de6adab2181e30e25eaca65a4e4e0328626fc3d359db`，233 个要求内建的配置项核对通过；实际 ARM64 内核加新 initramfs 在 QEMU 中进入用户态并输出可校验状态，EL1/EL2 交接回归通过。
- `armada4/read-boot.img` 为 99401728 字节（94.8 MiB，低于 96 MiB 限制），SHA-256 为 `6d564991bc61060344fd4772bd4ef4147a078080071d8519c84d936a309018b6`。四份负载及 ESP 内核/initramfs 解包逐字节比对通过。新 initramfs SHA-256 为 `774ca726ca1d2f16b3046d630748667fc623a4580954c67b2a46403be4af8b3a`。
- 实机 `initial/settled/final` 三份状态均完整校验通过，最终状态 SHA-256 为 `62b41dbb030bc40e5847283401caeaf8852ed75f15310c1f43b7e4907d3504f7`。确认 CPU `0-7`、内存 `15190292 kB`、msmdrmfb、NT36536 固件成功、ADSP/SOCCP 均运行；SCMI v2.0 与两个性能域初始化成功。
- 最终 UDC `a600000.usb` 状态为 `configured`，USB role 为 `device`，Type-C 为 device/sink。主机实际接收 127018 字节实时日志，SHA-256 为 `14de583a985371f9ad4deb6f72460f0a5a2478008adfe8ed0963cd1644d85c43`。这证明设备模式控制/数据通信；本轮没有测试 USB 主机模式或外接存储。
- 约 120 秒后自动复位返回 ColorOS，b 槽及 `boot_b` 哈希未变，用户确认解锁、指纹、Wi-Fi、KernelSU/susfs 全部正常。仍有 PCIe `cannot initialize host`、温度告警和 trace 节点的延迟探测；未把“USB/SCMI 已通过”扩大为“所有硬件已通过”。
- 已通过代码提交：`043c691`（Armada 显示/触控初始移植、日志恢复）、`5bd8ed8`（早期 PHY/CPUCP 内建、Zstd/25 份状态副本、USB 实时日志）。文档、私有固件、照片与原始日志均未提交。

### 19.6 第五轮：USB NCM 网络与 RAM 传输（B2 通过，2026-11-03）

- 用户选定试启动载体为 **USB NCM + RAM（B2）**。冒烟配置增加 `CONFIG_USB_CONFIGFS_NCM=y`；相对第四轮已验证配置只多 `USB_CONFIGFS_NCM/USB_F_NCM/USB_U_ETHER=y` 三项，233 项内建要求与 overrides 的 `=y` 全部复核。同一源码同一配置一次编出 `vmlinuz.efi`（SHA-256 `c8614378af38efa598a81346d029e1e5038c5282ffa2d43957bae72d752f7bfe`）、Image、DTB 与全部模块。冒烟 DT 重新生成后与第四轮逐字节一致。
- initramfs（`smoke_initramfs.py --usb-log --usb-ncm --wait 300`，SHA-256 `d7ce0e0f75760d2bfe4d7cb6dbae10c02998e4e6e0eb7ee0e30c9aa115660459`）：同一 gadget 带 ACM 日志与 NCM，平板端固定 MAC `02:4f:50:44:00:01`、地址 10.231.0.1，主机端 MAC `02:4f:50:44:00:02`。所有连接由主机发起：`nc -l` 接收 1 MiB 与 1 GiB，`httpd` 提供平板生成的 256 MiB 随机数据；平板只报告长度、整体及每 64 MiB 分块 SHA-256；主机结论送到 5009 端口后提前复位。主机脚本 `linux/ncm_test.py` 用 `nmcli ... save no` 的内存配置只匹配该 MAC、`never-default`，结束后删除。复现旧 initramfs 时 SHA-256 与第四轮 `774ca7…` 一致（触控固件需放在第一位）。
- 离线：用户态 netns + 同 MAC dummy 网卡 + qemu-user busybox 演练通过；人为改 1 字节的反例判为 FAIL。实际内核 + initramfs 的 QEMU 运行、U-Boot EL1/EL2 交接、四份负载及 ESP 回读比对通过。`armada5/read-boot.img` SHA-256 `6844c21a3a3cb52284d84584f04ce97cc76e229dbb84b0d9e869862a2ff2c8a9`（94.8 MiB）。
- 实机：fastboot 识别 12.7 s，0.01 s 后发出 boot。主机网卡 `enp0s20f0u5i2`；NM 未在 5 s 内自动激活，脚本兜底 `nmcli connection up` 成功。UDC high-speed，主机报 425 Mbit/s。结果全部一致：1 MiB（33 MiB/s）、1 GiB 16 块（71.1 s，14.4 MiB/s）、平板→主机 256 MiB 4 块（17.3 s，14.8 MiB/s）；主机 rx/tx errors 0，tx_dropped 34（链路拉起前）。平板收到 PASS 后提前复位，最终状态 7246 字节、SHA-256 `8c8dfd21…`（见 USB 日志）。
- 证据缺口：本轮 Android 只给出 `console-ramoops-0`，**没有 `pmsg-ramoops-0`**，25 份状态副本无法校验，原因未查（写入量与第四轮相近，远低于 2 MiB）。USB 日志从 kmsg 第 1431 条开始（主机等待 pkexec 授权期间之前的内容被丢弃）。本轮以主机端独立校验报告为主要证据。
- 显示：console 日志中 `DSI PLL(0) lock failed` 次数为 2/4/6（第二/四/五轮），本轮另有一次 `sending dcs data ff 20 failed: -110`；msm DRM 约 1.7 s 接管 fbcon，用户看到的闪烁/短暂黑屏对应这次接管。RAM 系统只有 fbcon，没有图形会话属于预期。DSI 告警留到 D 步排查。
- 返回：b 槽，`boot_b` 哈希不变，用户确认解锁、指纹、Wi-Fi、KernelSU/susfs 正常。代码提交 `e2e2277`。证据 `armada5/{ncm-report.json,hardware-validation.json,usb-console.log,console-ramoops-0,launch.json,android-return.json}`。

### 19.7 A：完整内核包与 Armada rootfs（进行中）

- 内核包：`armada-port/kpkg/out/armada-kernel-7.2.6.tar.zst`（SHA-256 `d11f34c5ca0487a9b078f84a73edb398cefd3d8440e5194248da5fe482921e10`，46.7 MB），布局同 `build-kernel.sh`：`lib/modules/7.2.6/{vmlinuz,模块,dtb/qcom}`，1594 个模块、29 份 DTB；`.armada-source` 注明这是含冒烟内建项的测试构建。与第五轮上机的 `vmlinuz.efi` 同源同配置。
- 本地包镜像：ghcr 上按当前源码哈希只缺 `kernel` 与 `openabl-updater`，其余 32 个直接拉取。`~/.docker/config.json` 中失效的 ghcr 凭据会导致 403，构建时用 `REGISTRY_AUTH_FILE` 指向空认证文件。`openabl-updater` RPM（SHA-256 `2db6bcae…`）已核对含 `oneplus-pad-2-pro` 排除逻辑；`device-env` 以型号 “OnePlus Pad 2 Pro” 选中该配置。两个本地镜像均为 `FROM scratch`，需标为 `linux/arm64` 平台。
- 新版 podman 5.8 不接受 `ARG *_REF` 写在 `FROM` 之间再用于后续 `FROM`（CI 的旧版 buildah 可以）；本地使用把这些 ARG 移到开头的副本 `rootfs/Containerfile.globalargs`（行集合与原文件相同），并加 `--skip-unused-stages=false --security-opt label=disable`。仓库 Containerfile 未改。
- C 的 RAM 启动需屏蔽：`mtp-gadget`（抢 USB gadget）、`armada-bootimg-update/finalize`、`armada-boot-update`、`armada-efi-update`、`armada-installer`、ABL 更新等写启动分区或安装的服务；Linux DT 仍禁用 UFS。

### 19.8 第六轮：完整 Armada 从 RAM 启动（C 基本通过，2026-11-03）

- rootfs：本地构建 `localhost/armada-opd2413:test`（约 33 分钟，qemu-user 模拟 arm64；版本 `20261102.5bd8ed8.opd2413test`；镜像内 `vmlinuz` = 第五轮 Image `4102cdf8…`），`podman unshare` 导出为 zstd squashfs（1 MiB 块，保留属主/xattr）6702510080 字节，SHA-256 `60809a2544d5a52fecf2885ad2c484aaf1238f3d6d91af94bd3c4594a8eb85a9`。
- 启动方式（提交 `15f36a5`）：initramfs 经 NCM 收镜像到独立 tmpfs，长度+SHA-256 与内置值一致才挂载；squashfs 只读 + tmpfs 可写层 overlay；注入测试树（屏蔽 17 个写启动分区/改分区/更新/抢 USB gadget 的 unit；NM 不管理 NCM 网卡；journal 转发 kmsg；busybox 状态页；pmsg 状态；计时器或主机请求后回 Android）；`selinux=0`。systemd-boot 两项：默认带本机 GPU zap（`/vendor/firmware/gen80000_zap.mbn`，SHA-256 `8e4e5da8…`，OPLUS 签名，与镜像中 linux-firmware 测试签名版不同），`opd.nofw` 项不带。
- QEMU 预演（virtio-net 同 MAC + 端口转发）发现并修复：busybox `wc -c` 在 4 GiB 回绕（改 `stat -c %s`）；镜像已自带两个 mask 链接，`cp -a` 不覆盖（改 `cp -af`）；主机等待校验超时过短。QEMU 中到达 graphical.target，gamescope 因无 Vulkan 设备退出（预期）。
- 实机：换主机 USB 口后传输 **35 MiB/s**（3 分钟；第五轮 14.5 MiB/s 的瓶颈在主机端口）；平板 Linux 仍为 high-speed，同一口 Android 下协商 5000 Mbps（USB 3 硬件/线缆可用，Linux 缺 USB3 PHY）。systemd 到 `graphical.target`（degraded：`armada-controller-type`、`armada-flatpak-setup` 失败）；SDDM 自动登录 → gamescope → Steam；用户完成 Steam 首次设置（中文、时区、Wi-Fi）和客户端更新，会话随后重启一次（非系统重启，uptime 连续）。GPU：gen80000 SQE/GMU v5.1.25/AQE 加载，gamescope 合成正常（渲染器名未记录）；DRM 3392×2400；Wi-Fi `wlp1s0`（ath12k/PCIe）取得 DHCP 地址；磁吸键盘可用。
- 问题：①会话重启（面板重新 prepare）后触控失效，`nt36536` 重新请求 `novatek/nt36536-xn242.bin` 得 -2——该固件只在 initramfs，需加入 rootfs/注入层；②启动画面切换中文后乱码（armada-splash 字体缺 CJK 字形，属 Armada 本身）；③MemAvailable 约 1.3 GB（6.4 GB 镜像占 RAM）；④主机脚本错过 initramfs 校验页，按超时提前发结束命令（已修：以完整系统状态页推断）；保存的 journal 被空响应覆盖（已修）。USB 日志（2574 行，含转发的 journal）保留了完整过程。
- pmsg 本轮存在（649415 字节），解码器增加 `handoff/booted` 阶段后，`booted`（`a737bf70…`）与 `final`（`63d6e075…`）SHA-256 校验通过；initramfs 阶段副本未保留。
- 返回：b 槽，`boot_b` 不变，用户确认解锁、指纹、Wi-Fi、KernelSU/susfs 正常。未写入平板存储；私有 zap/触控固件只在 `output/`。证据：`armada6/{hardware-validation.json,verified/,usb-console.log,launch.json,android-return.json}`。

### 19.9 第七轮：触控固件 + 诊断（显示故障，2026-11-03）

- 同一 rootfs（硬链接复用），注入层增加 `novatek/nt36536-xn242.bin` 与 GPU/触控诊断（仅 booted/final 阶段运行 vulkaninfo/eglinfo/libinput），测试窗口 2700 s；`armada7/read-boot.img` SHA-256 `27e8f3424a4d31aec9b45fdf944b53f3f3846c7923789934b64ae1473362ac84`，离线检查同第六轮。
- 实机：传输回落到 **14.8 MiB/s**（同一主机端口；第六轮 35 MiB/s，速度差异来源未知，不是端口）。用户看到传输开始前有内核日志，**传输开始起即黑屏**，直到结束。内核 `consoleblank` 默认 0，排除控制台熄屏。
- 完整系统照常到 graphical.target；gamescope 选中 Vulkan 设备 **`Adreno (TM) 830`**（硬件渲染，E 的关键证据）；armada-splash 崩溃（ABRT）后前两次 gamescope 落到 headless 后端并被会话脚本 SIGKILL，第三次正常取得 DSI-1 3392x2400@120Hz；Steam CEF（8080）一直未就绪。无 OOM（MemAvailable 3.8 GB）。电源键短按交给 Steam（未休眠）。
- 黑屏证据：关机过程中 USB 日志持续出现 `dpu_encoder_phys_vid_wait_for_commit_done ... vblank timeout` 与 `wait for commit done returned -110`，DPU 视频模式管线停止；nt36536 探测时 `panel_on=0` 且始终未加载固件（面板未经 DRM 点亮）。DSI PLL lock failed 4 次、`dcs ff 20` -110 1 次，与以前能显示的轮次相近，不能单独解释。
- 假设（待验证）：黑屏与低速都始于传输开始，可能同源——互连带宽/时钟状态（`sync_state() pending`、`clk_ignore_unused`）在部分启动中不足，USB DMA 与写 RAM 时 DPU 取数不足而停管线。下一轮需要可交互调试：在 NCM 上提供只绑定 10.231.0.1 的 busybox telnetd，检查背光、DRM/DPU 状态、interconnect 与时钟。
- 结束：主机发结束命令后关机较慢（每次提交等超时），约 4 分钟后自行复位回 Android，未强制重启。本轮 Android 未给出 pmsg。b 槽、`boot_b` 不变，用户确认 Android 四项正常。证据 `armada7/{live/,usb-console.log,launch.json,android-return.json}`。

### 19.10 第八轮：调试 shell、现场补固件（2026-11-03，提交 `a4d314b`）

- 新增 `--debug-shell`：initramfs 与完整系统各一个 busybox telnetd，仅绑定 `10.231.0.1:23`（实测 `ss -ltn` 只在该地址），`switch_root` 前结束 initramfs 的实例；主机 `linux/opd_shell.py` 拒绝全部 telnet 选项、按随机标记切分输出。镜像 `armada8/read-boot.img` `e71ec206…`。上机脚本这一轮只启动和收日志，传输由手动启动的 `netroot_host.py` 在采完传输前快照后开始。
- 显示基线（正常启动）：背光 1433/2047、`bl_power=0`，DSI-1 connected/enabled/On；`disp_cc_mdss_mdp_clk` 575 MHz、byte0 170 MHz、pclk0 227 MHz；interconnect 中 display-subsystem 申请 avg 5193179/peak 800000、USB 1000000/2500000。传输全程 **35 MiB/s**，MDSS 中断每 20 s 约 196 次稳定增长，屏幕一直有内核日志。快的两轮（6、8）显示正常、慢的一轮（7）黑屏，仍符合“同源”假设，但本轮未复现故障，无对比数据。
- 用户按“第一项”选到了 `opd.nofw` 项（systemd-boot 默认高亮在“自动重启”项，位置描述有歧义；以后只按标题指示）。无 OnePlus zap 时 `adreno_zap_shader_load ... -2`、`gpu hw init failed: -2`、`GMU OOB set GPU_SET` 超时，gamescope 起不来，SDDM 反复重启会话（到第 46 个），屏幕退回内核日志——反向证明本机必须用 OPLUS 签名 zap。
- 现场修复：主机入站 HTTP 不通（主机侧 nftables 规则，未深究），改为经调试 shell base64 下发 zap（`8e4e5da8…`）与触控固件（`3e359689…`，分块）到 RAM 可写层，`systemctl restart sddm` 后无 zap/GPU 错误，gamescope 用 Adreno 830 进入 3392x2400@120，用户完成语言/时区/Wi-Fi，**触控与磁吸键盘可用**；libinput 识别 `NVTCapacitiveTouchScreen` 与磁吸键盘触控板（120x71 mm，手势）。约 830 s 出现两次 `vblank timeout` 但画面继续。
- 提前结束：uptime ≈990 s 有连接打到 5009，`opd-return` 写最终状态后正常重启（关机很快）。原因是 `opd-control` 用的 busybox nc 不能绑定地址，Wi-Fi 接入局域网后端口对局域网开放（推测为路由器扫描）。已改为 python3 只监听 `10.231.0.1:5009` 并记录连接方（本地测试通过，待实机）。本轮 Android 未给出任何 pstore；证据为 `armada8/{diag-*.txt,usb-console.log,ncm.out}`。b 槽、`boot_b` 不变，Android 四项正常。
- 下一轮：同镜像 + 控制端口修复；按标题选「debug shell」项；先过 Steam 更新后的会话重启验证触控，再用 `steamos-session-select plasma`（armada 用户）进桌面测触控映射/方向/多指。

### 19.11 第九轮：D 通过；桌面模式受阻于 kwin/kglobalacceld 版本不匹配（2026-11-03）

- 镜像 `armada9/read-boot.img` `c13ed482…`：控制端口改为 python3 只监听 `10.231.0.1:5009`（实机 `ss` 确认，结束命令生效；提交 `25691d2`）；启动项按标题区分「Armada 9 WITH firmware (debug shell)」/「… without firmware (fallback)」，用户选对。传输 35 MiB/s，显示全程正常。
- E 证据（booted 状态）：Vulkan `Adreno (TM) 830`，`turnip Mesa driver` / Mesa 26.2.5，API 1.4.354；EGL/GL/GLES 渲染器均为 Adreno 830。libinput：`NVTCapacitiveTouchScreen`（identity 校准矩阵）与 `OnePlus Pad 2 Pro Keyboard Touchpad`（120x71 mm，pointer+gesture）。
- 触控修复确认：Steam 更新后会话重启，`nt36536 resume #2` 从 rootfs 加载固件成功（`Update firmware success`）。
- 桌面模式（`steamosctl switch-to-desktop-mode`，经 `/usr/libexec/os-session-select plasma` 以 armada 用户执行）首次失败：`kwin_wayland: symbol lookup error: libkwin.so.6: undefined symbol _ZNK13KGlobalAccelD9interfaceEv`。镜像中 `kwin-6.7.5-1.fc44.armada`（Armada 包，2026-10-07 构建）+ Fedora 更新源的 `kglobalacceld-6.8.2`（无该符号）；6.6.4 有符号但要求 Qt 6.10 私有 API（镜像 Qt 6.11.2）。**这是 Armada 当前构建的通用问题**（`@kde-desktop` 未锁版本），与 OPD2413 无关；需上游锁定 Plasma 组件或把 kwin 包升到 6.8。
- 现场验证：Koji `kglobalacceld-6.7.5-1.fc44.aarch64.rpm`（SHA-256 `b29586c2…`，digests OK，Koji 包无 GPG 签名）依赖 Qt 6.11 私有 API 且导出该符号；经调试 shell 装入 RAM 层后桌面模式启动（kwin_wayland + plasmashell）。途中 RAM 可写层（3 GB）被 Steam 更新包占满导致传输/dnf 写入失败，删除 `Steam/package/*.zip*` 后恢复。
- **D 通过**（用户实测）：桌面模式点按、拖动、四角定位、多指缩放/滚动均正确，方向正确；磁吸键盘可用。
- 操作失误记录：用户点的桌面“Steam 图标”实为 Armada“返回游戏模式”快捷方式；我误以为是桌面版 Steam 而强杀，导致游戏会话重启、启动画面显示“steam launch failed”，随后切回桌面恢复。
- 返回：结束命令后正常复位；Android 未给出 pstore；最终状态 SHA-256 `5d00f4a3…`（USB 日志）。b 槽、`boot_b` 不变，Android 四项正常。
- 下一步候选：镜像构建时修正 kglobalacceld（本地先锁/替换为 6.7.5，同时向上游报告）；可写层扩容或把 Steam 数据目录挪到单独 tmpfs；F/G 外设（音频、充电、蓝牙、手写笔、相机）；USB3 PHY；H 稳定性；I 安装。

### 19.12 第十轮：USB 3 与黑屏根因（2026-11-03）

- 镜像修复：在 `localhost/armada-opd2413:test` 上叠一层装 Koji `kglobalacceld-6.7.5-1.fc44`（SHA-256 `b29586c2…`，构建时校验并确认 kwin 无缺失依赖），得 `test-kga`；squashfs 6702497792 字节，SHA-256 `35d58440f256d5b300f028117e816b1bc5cfa13b1311664b4b04ed8d4486ffac`。RAM 可写层改为 5 GB。`armada10/read-boot.img` `23b336c5…`。
- **USB 3**：UCSI 正常（`/sys/class/typec/port0` 与 partner；`7-000e`=WCD9395 USBSS 与 `88e8000.phy` 的 mux/switch 均注册；UDC `maximum_speed=super-speed-plus`）。原插法下 `high-speed`（主机 480M）；**平板端插头翻转 180° 后 `super-speed-plus`，主机 10000 Mbps（USB 3.2 Gen 2）**。结论：硬件/驱动链路可用，缺的是把 UCSI 方向传给 QMP PHY/WCD9395（PHY 固定一种方向）。待修。
- NCM 吞吐不受链路速率限制：SuperSpeed 下 17.4 MiB/s；`irq/226-dwc3` 线程占约一个核，dwc3 中断全在 CPU0；改到 CPU7 仅 18 MiB/s。
- **黑屏根因（本轮现场采集，`armada10/diag-black.txt`）**：与第八轮正常基线比，背光、DSI-1 connected/On、MDP 575 MHz、interconnect 申请均相同，但 **两路 `dsi_isr` 计数 0/1（正常 122/122）**；启动时 `DSI PLL(0) lock failed` ×4、`ae96000.dsi: dsi_get_phy: phy driver is not ready`、首条 DCS `ff 20` 超时 -110，**无 `panel prepared`**（正常轮 1.63 s 出现）。之后 115 次 `vblank timeout`/`LUT load timed out`。停启 SDDM 后 DSI 中断仍为 0，modeset 不能恢复。即双 DSI（ae94000+ae96000，PLL0 共用）启动初始化竞态，约 2/5 轮出现（7、10；6、8、9 正常）。
- 低速与黑屏相关：显示正常的轮次 35 MiB/s，黑屏轮次 15–18 MiB/s；推测为 DPU 超时/控制台提交与 dwc3 中断线程争用 CPU0（待证）。修复需深入 DSI PHY/PLL 初始化顺序或面板 prepare 失败后的重试。
- 结束后关机约 20 分钟才复位（显示管线卡住），自动重启回 Android，未强制。b 槽、`boot_b` 不变，Android 四项正常。
- U 盘启动准备：用户 64 GB Samsung Type-C U 盘（脚本按序列号定位，原 exFAT 内容经用户确认不备份）已重建为 GPT+ext4（卷标 `OPDARMADA`），`opd/root.squashfs` 清缓存回读 SHA-256 一致，`opd/upper`、`opd/work` 作持久可写层。新增 `--usbroot`（initramfs 按卷标等待 U 盘、校验、overlay 可写层在 U 盘、挂载点移入 `/opd-usb`）与 `--ssh-key/--no-ncm`（sshd 仅密钥登录，专用密钥 `~/.ssh/opd2413_ed25519`）。QEMU（virtio 盘）演练通过。第十一轮镜像 `armada11/read-boot.img` `0ea4f186…`。

### 19.13 第十一、十二轮与开机黑屏压力测试（2026-11-03）

- **U 盘启动（第十一轮）成功**：USB 主机模式可用（role host，平板供电），从 `OPDARMADA` U 盘启动，可写层持久，SSH（专用密钥 `~/.ssh/opd2413_ed25519`）经 Wi-Fi 可用；U 盘在原插法下 480 Mbps。运行中切 60 Hz 后黑屏、切回 120 Hz 仍黑（面板命令均发出、无 DPU 报错；KWin 用 1D+3D LUT colorop 管线，待查）。系统运行中拔 U 盘会导致根文件系统丢失（第十二轮发生过一次，随后自动回 Android）。
- **镜像修复**：本地镜像 Plasma 6.7.5/6.8.2 混装（Armada 自编 kwin/powerdevil 等 6.7.5，`@kde-desktop` 从 Fedora 更新源装入 45 个 6.8.2 包）→ kcm_kscreen 缺 `KScreen::Log::instance()`、kwin 缺 `KGlobalAccelD::interface()`。叠层用 `updates-archive` 统一降到 6.7.5（`localhost/armada-opd2413:test-plasma675`，207 个 Plasma 插件全部可加载），squashfs SHA-256 `c9765bfe…`，已写入 U 盘（保留可写层；删除了可写层中旧 rpmdb WAL/SHM）。属 Armada 上游构建问题，需上游锁版本或升级 kwin 包。新增 `opd.desktop` 启动项（drop-in 让 `armada-session-default` 执行 `session-control default-desktop`）。
- **开机黑屏压力测试**：`smoke_initramfs.py --display-probe`（25 s 时记录面板状态经 NCM 提供后复位）+ `linux/stress_display.py`（每轮只读预检查 → fastboot boot → 收结果 → 回 Android）+ `uboot/build.py --unattended`（跳过三键确认，否则无人值守时会直接回 Android）。有效样本约 22 次中 6 次黑屏（约 30%）。
- 已排除：面板本身（面板驱动 3 次上电重试全部失败）；interconnect sync_state（25 s 内未发生）；ADSP 上线时机（多次好样本 ADSP 更早）；PLL0 未锁（坏样本中 PLL0 在第一条命令前已锁定，1.36 GHz，带从属）；PLL 失败路径清理与链路时钟使能检查（修复后仍黑屏）。
- **定位**：双 DSI 同步广播（`sync-dual-dsi`，DSI1 触发、DSI0 跟随）中，坏样本第一条 DCS（`ff 20`，或中途某条）时 DSI1 `STATUS0=00020003`（命令 DMA 忙）、FIFO 全空、`CLK_STATUS=008077c3`；DSI0 `STATUS0=00000000`（空闲）、`CLK_STATUS=00006343`（动态字节/像素/命令像素时钟未运行）。驱动超时后不复位 DSI 主机，后续重试都卡住。另见：Armada 补丁使 SM8750 每条命令都重设/开关链路时钟（只对 sm8250 跳过）；PHY 注册时孤儿时钟重挂接导致 4 次早期 PLL 锁定失败和 `already unprepared/disabled` 计数警告（好坏样本相同）。
- 已整理为补丁：`1403`（面板 prepare 断电重试 3 次）、`1404`（DSI 7nm PLL 锁定失败时撤销启动与偏置）、`1405`（命令 DMA 超时转储两路 DSI 寄存器、链路时钟使能失败报错，仅错误路径输出）。调试期间的 PLL 锁定日志与首次失败调用栈未纳入。证据：`stress1/2/4/5/runs/`。
- 待选方向：①超时后对两路 DSI 主机做完整断电/复位/重配置再重试；②开机首个 prepare 沿用 ABL 已点亮的面板（不复位、不发初始化序列，类似 continuous splash）；③对照厂商 SDE 驱动的双 DSI 命令广播流程（DMA 触发顺序、同步配置）。

### 19.14 开机黑屏：显式硬件广播候选（2026-11-03，离线准备）

- 目标：消除 19.13 的命令 DMA 启动竞态；不引入延时、重试或连续显示绕过。Linux 接管的显示故障与 kShimC 菜单区分验证。
- 原始证据：`stress5/runs/dmesg-05.txt` 首条 `ff 20` 超时，DSI1 `STATUS0=0x20003`；当前 `dsi_ctrl_enable` 从未设置 `CMD_DMA_CTRL.BROADCAST_EN`，manager 直接先触发 DSI0 再触发 DSI1。
- 复用：OnePlusOSS SM8750 Android 16 的 `dsi_display_broadcast_cmd` / `dsi_ctrl_hw_cmn_kickoff_command` 明确配置 bit31 广播、bit30 主端，先 stage 两端，再触发从端、最后主端；只在主端等完成 IRQ。另有同 SoC Lenovo 开发者在 2026-11-02 的 dri-devel 原始回复报告相同 `0x20003` 症状（线索，不能代替本机实测）。原始源码保存在 `/tmp/opd-vendor-*`。
- 单变量实验：保留 stress5 的内核配置、DT、initramfs、诊断与已证伪的旧重试，以 `broadcast1/before/` 保存源码和二进制；只替换 SM8750 同步写命令的 DMA 广播路径。完成后须去掉面板盲重试等临时诊断。
- 判据：面板第一次 prepare 成功、DCS/DMA/vblank 超时为 0，显示提交后两端硬件均完成；重复临时启动对照（目标至少 20 次）且每轮 Android 槽位/boot 哈希不变。若同样失败两次，停止当前候选，回到最早错误层。
- 用户已授权本轮最多 20 次自动临时启动并确认其他任务已停止、由当前 agent 独占设备。镜像 SHA-256 `9808892870cc1df646406653b098f96790a59a32853cbc70265963ae8f2fbbb3`；负载比对、独立审查、实际 C 事务的寄存器模型正反例通过。QEMU 原 ESP 默认 Linux 的菜单等待超过测试期限，因此不算通过；仅把 loader 默认改为 reboot 的等价 ESP 回归 EL1/EL2 Android 交接通过（不证明 DSI）。开始前 Android b 槽/boot 哈希正确、电量 100%。
- 首次编排未发送候选：`adb reboot bootloader` 后 sandbox 的 `/dev` 视图没有新 USB 节点，脚本等 fastboot 90 s 超时，设备停在原厂 START。主机权限环境确认 b 槽后正常 `fastboot reboot`，Android 原 boot 哈希不变。后续编排在主机实时 USB 环境执行；这是主机通道失败，不计入显示样本。
- 注意：另一任务在准备期间把 1403/1404/1405 提交到了 series，并删去了 PLL 成功/调用栈日志；候选保留实际编译到的 PLL 失败清理，不把这些早期告警改变当作广播修复证据。源码哈希与差异保存于 `broadcast1/`。

### 19.15 显式广播未修复；控制器复位时钟域差异（2026-11-03，失败；见 19.16）

- `broadcast1` 第 1 轮通过（122 次主端完成、从端轮询通过），第 2 轮首条 `ff 20` 失败，自动停止并回 Android；两端 IRQ 0，主端 `STATUS0=0x20003`。证据 `broadcast1/runs/`。广播位与触发顺序不是本机黑屏的充分修复，候选未加入 series。
- 新的更早差异：厂商 `dsi_ctrl_hw_cmn_soft_reset` 在复位前清除 CTRL 的 DSI/VIDEO/CMD 三个引擎位，并临时设置 `DSI_CTRL_DYNAMIC_FORCE_ON = 0x23f | BIT(8) | BIT(9) | BIT(11) | BIT(21)`；主线只清 DSI_EN，CLK_CTRL 只写 `0x23f`。门控的字节/像素域可能没有收到复位，这是待实验证伪的假设，不能仅凭厂商差异宣布根因。
- 单变量 `reset1`：撤销整个广播候选，恢复旧同步 DMA 路径；仅对 SM8750 按厂商顺序临时打开复位所需动态域，1 us 脉冲后还原 CLK_CTRL 与 CTRL，不永久关门控、不加启动延时、不做命令重试。原 1403 重试暂留作对照，成功判据仍要求第一次成功。其余 SoC 维持原复位路径。
- 判据同 19.14。该候选后由另一任务实测失败，见 19.16；不再次重复。

### 19.16 接续另一任务的 reset/vid/diag 证据（2026-11-03）

- 另一任务已跑过相同 reset1 镜像：第 1 轮第 5 条命令 `fb 01` 失败，两路各 4 IRQ；`reset2` 未上机。`vid1` 延后 VIDEO_MODE_EN 到 bridge enable，仍首条 `ff 20` 失败，CTRL=0x1f5 证明视频位确实关闭。两个候选都不足以修复，不能推广为这些机制在所有场景都无关。
- `diag1` 首轮通过、第 2 轮失败；原始证据均在 `output/opd2413-stage4/{reset1,vid1,diag1}/runs/`。从另一任务用户给的记录接续，当前没有运行中的压力脚本；后续由本任务独占设备。
- 已自行重新解析 diag1 全部寄存器，每阶段每路 255 个 32 位值（缺失不能当零）：正常/失败的 pre-init 和 pre-trigger 全部一致。失败从 pre-trigger 到 timeout，DSI0 仅 DMA_BASE/LEN、TRIG_DMA、未知 0x394 改变；DSI1 额外 STATUS0=0x20003、CLK_STATUS=0x8077c3。解析结果在 `diag1/parsed-registers.json`。
- 更正：STATUS0.INTERLEAVE_OP_CONTENTION 是 bit31，不能把 0x20003 的 bit17 说成视频争用；CLK_STATUS 的动态位反映活动状态，单独为 0 不证明 CCF 链路时钟被关闭。广播单独失败不等于广播无关。
- 新的单变量 `clkxfer1`：恢复 stress5/1405 基线，保留原同步 DMA；仅在 SM8750 的命令事务范围内，按厂商 0x200b3f 掩码保持动态域运行，覆盖 CMD_MODE_EN → DMA → 恢复 CTRL，随后还原原始 CLK_CTRL。与 reset1 的区别是覆盖真正命令事务，而非只覆盖复位脉冲。不全局禁用门控、不增加延时或重试。假设：触发/命令状态跨动态域时缺少运行条件，软件可读配置一致但内部状态机未接收控制更新。
- 通过标准：首次 prepare 成功，DCS/DMA/vblank 超时均为 0；最终需要无重试和最小化日志的重复启动回归。独立审查、实际 C prepare/restore 的 100 次 LP/HS 状态恢复模型和非 SM8750 对照、负载解包比对均通过。镜像 SHA-256 `b537083cf00e391d03949bec030c1cd7dc63dd72cbd4a5b90e56ca2e0c9c705c`。
- 用户已授权本轮后续局部候选累计最多 20 次 `fastboot boot` 自动验证，设备由本任务独占，其他任务保持暂停。该授权覆盖同一黑屏排查内的后续局部候选，不包含任何刷写/切槽。先做至多 6 次 clkxfer1（首次失败就停止），为无重试最终候选保留测试次数。开始前 Android b 槽/原 boot 哈希正确、电量 100%，温度约 31°C。

### 19.17 clkxfer1 失败；更早的 SMMU 地址翻译故障（2026-11-03）

- clkxfer1 第 1、2 轮通过，第 3 轮首条 `ff 20` 失败；已停止、回 Android。仅命令期间保持动态时钟域开启不足以修复，`clkxfer2`（增加错误传播/锁和去重试的离线整理）不再上机，不加入补丁栈。本轮 20 次授权已用 3 次。
- 重新检查完整日志发现原先“无 SMMU context fault”的说法不成立：所有样本启动约 0.7 s 已有 SID 0x800 对 `0xfc8xxxxx`（ABL framebuffer）的翻译失败；部分失败样本 1.49 s 在新 CB=9 再次访问原 framebuffer 触发 fault。其余样本早期已用尽 rate limit，不能用后续未打印就当作无 fault。好坏样本均有早期 fault，这本身不证明是黑屏根因，但它早于 DSI 且是确定的地址域错误。
- 主线代码证据：SM8750 只在 `qcom_smmu_actlr_client_of_match` 有条目，却缺少 `qcom_smmu_client_of_match` 的 identity 默认域匹配。MDSS DT 是 `qcom,sm8750-mdss`，`iommus = <&apps_smmu 0x800 0x2>`。现有其他机型表项用于在 DRM 接管前保留 bootloader 的物理地址扫描；之后 DRM 仍可附加自己的翻译域。不是全局禁用 IOMMU。
- 单变量 smmu1：恢复 1403/1404/1405 基线，撤销全部 reset/vid/clock/broadcast/diag 候选，仅在 identity client table 加一条 `qcom,sm8750-mdss`。判据：早期 SID=0x800 的 ABL framebuffer fault 消失，同时首次面板 prepare 和两路 DMA 正常。若 fault 消失而仍黑屏，记录该故障已修复但不足以解决黑屏，继续追踪接管时切域。
- smmu1 已编译并完成 4 份负载解包比对；镜像 SHA-256 `c67ab5a69edce56f78165a9c6d6b04b7361a95c74ec2ac169f563e0bcf069daa`，内核 `720f6cfeade4480a87ba80263e9d81c7ce73a4f8bbf81f0c34877dfb15ba1641`。先做最多 3 次临时启动，计入剩余 17 次授权。

### 19.18 默认域已修复，但 KMS 交接后仍读旧帧缓冲（2026-11-03）

- smmu1 第 1 轮显示失败，自动回 Android，原槽/boot 哈希不变。早期约 0.7 s 的全部 fault 消失；1.496089 s 仍有 SID 0x800、IOVA 0xfc89ba00 的单次 fault（paging CB=8），1.696907 s 首条 DCS 超时。默认域条目确实生效，但不足以覆盖 DRM 已换页表之后的硬件访问。
- 当前最早的可观测故障已从早期 default DMA 域收敛到 KMS 接管窗口。现有 dpu_kms_hw_init 在分配和配置显示资源前调用 _dpu_kms_mmu_init；MSM MDSS 早期全局 reset 不能用作“扫描已停稳”的实测证明。
- 单变量 smmu2（在 smmu1 基础上）：SM8750 DPU 在换域前，依 catalog 地址写两路 DSI INTF_TIMING_ENGINE_EN=0，再轮询 INTF_STATUS.bit0 至两路实际关闭（上限 100 ms，失败拒绝换域）。该 status 与 dpu_hw_intf_get_status 对 >=5 代核心的定义一致；正常 dpu_encoder_phys_vid_disable 也必须等 disable 在 vsync 锁存。没有固定启动 sleep、重试或永久旁路。
- 判据：打印原 active mask、停机必须完成；KMS 换域后不再 fault 旧 framebuffer，首次面板 prepare 成功，两路 DMA 正常。若 active mask 本来为 0 而仍 fault，则还需找旧读请求或非 INTF 触发来源，不能继续把它归咎于扫描未停止。本轮授权已用 4/20 次。
- smmu2 编译、独立审查、实际 C helper 的双接口先停后等/其他 SoC与DP不触碰/超时传播模型、负载比对通过。镜像 SHA-256 `814c876866241d44e0aeb9373856d41649024b675d599679e2bc3e617ac28874`，先最多 3 次，首次失败停止。

### 19.19 INTF 已关闭仍有旧读请求：清理 CTL 取数路径（2026-11-03）

- smmu2 第 1、2 轮通过，第 3 轮仍失败；三轮启动前实际 INTF active mask 都为 0。失败在 1.351716 s 对旧 framebuffer 0xfc89bc00 fault，1.552466 s 首条 DCS 超时。不能再把问题仅归因于 timing engine 未停稳。证据 `smmu2/runs/`；本轮累计 7/20 次。
- 主线历史讨论（Bjorn Andersson / Dmitry Baryshkov，2021–2022，Clear/Disable boot loader configured data paths）区分 INTF 时序与 CTL 取数路径：旧 CTL_FETCH_PIPE_ACTIVE 可能保留，不由一个 idle INTF 状态证明取数路径已退役。当前驱动已有完整 encoder teardown 对 CTL 的 clear_all_blendstages、set_active_fetch_pipes、set_active_pipes、set_active_lms 和有界 reset 操作。
- smmu3：只对 SM8750 把 paging attach 延后到资源对象创建之后，在原 INTF 停机基础上复用上述 CTL teardown，6 条路径都成功 reset 后才 attach；任何 reset 失败拒绝换域。其他 SoC 的初始化顺序不变。保留已验证的默认 identity 条目。
- 判据：CTL 清理/复位完成，换域后不再访问旧 framebuffer，首次面板/DMA 正常；否则停止“旧 CTL”方向，重新验证 fault 请求来源。独立审查、编译与负载比对已通过；镜像 SHA-256 `24011964a36f4c0756a8d5daeb04c0a7ff03e724ade22b008ef7f8fea2523972`，开始至多 3 次实机验证。

### 19.20 CTL 退役候选通过三轮；最终回归第 8 次失败（2026-11-03）

- smmu3 3/3：面板首次成功、DSI IRQ 122/122、无 SMMU/DCS/vblank fault；6 个 CTL 的清理与 reset 都完成。每轮自动回 Android、b 槽/boot 哈希不变。源码与原始日志在 `smmu3/`；本轮累计 10/20 次。
- 最终候选 `handoff-final`：同样的 RM → INTF 停机 → CTL 退役/复位 → paging attach 顺序，限定 root compatible `oneplus,erhai` 且 DPU `qcom,sm8750-dpu`；其他板保持原初始化顺序。移除全部成功诊断打印，删除无效 1403 面板盲重试（面板源码回到原始 1400 版本）。默认 identity 域匹配仍适用于 SM8750 MDSS，不影响正常 KMS 翻译页表。
- 已整理为 `1406-drm-msm-retire-opd2413-firmware-fetch-before-iommu.patch`，零 fuzz 应用后逐字节等于实际构建源码；主机真实 C helper 的板/SoC 门控、双接口先关、6CTL清状态后reset、INTF/CTL超时传播检查通过。未添加 Signed-off-by 以免代替人类签名；checkpatch 以 no-signoff 模式检查代码风格。
- 观测 initramfs 保留全部原固件/BusyBox，唯一变化是 25 s 时的 `panel_on` 从重试成功文本改为面板 follower 的 `panel prepared`；逐项 cpio payload 比对、shell 语法检查通过。不会影响首次显示的启动时序。
- 最终 boot SHA-256 `e15d09b762fd7a26a3a1efbdf3a7fcdcb4d0501029d05d6039702b043407694c`，kernel EFI `3d382df73f160196d0df67a3d1a303dac2068d95d4296d05afaedcafe8962b0d`。计划最终版本共 10 次回归，首次失败停止；另强制要求完整 dmesg 含 Linux/用户态启动标记且 SMMU context fault 为 0。尚不能把 3 次候选通过说成最终已修复。

### 19.21 暂停记录与恢复（2026-11-03）

- 用户要求暂停时，最终镜像已完成 4 次自动循环，第 5 次已经启动。停止后续编排，单独采集第 5 次的显示和完整 dmesg，再确认回 Android；恢复时原进程已退出，`sys.boot_completed=1`、槽位 `_b`、原 boot_b SHA-256 正确。第 5 次证据：`handoff-final/pause-current-{display,dmesg}.txt`、`pause-current-validation.json`。
- 最终版当前 5/5 通过：DSI IRQ 122/122、首次面板 prepare、SMMU context fault/DCS/vblank 错误均为 0。早期 PLL lock failed 4 次和 probe defer 仍存在，未随黑屏修复消失；不能把它们直接当成黑屏根因。
- 已确认 SM8750 MDSS 缺失默认 identity 域匹配会导致早期固件 framebuffer 地址访问 fault；仅补此项、或只停止 INTF timing，都仍出现新 paging 域访问旧 framebuffer 的 fault 和随后 DCS 超时。当前最强证据支持：切换 KMS 页表前还必须清退固件遗留 CTL 取数路径。完整清退候选 smmu3 3/3、最终无重试版目前 5/5 通过，仍待完成回归，不能宣称所有冷启动场景已根治。
- 用户要求继续后，执行剩余 5 次相同最终镜像回归，使用新目录 `handoff-final/resume-runs/` 保存，避免覆盖暂停前的日志；本轮局部候选总上限仍为 20 次。
- 已离线准备完整 USB/rootfs 使用的 `armada13/read-boot.img`，SHA-256 `e297217883181d4c26b5720337c8e7df693b0c0c313db7d1132a4f4831651ed9`；kernel EFI 与最终探针一致。相比 armada12，ESP 仅替换 kernel，菜单默认 ColorOS；各 payload 逐字节核对通过，见 `armada13/offline-validation.json`。**此镜像尚未上机，不能把探针通过视作完整桌面验证。**
- 工作树保留 1406 补丁、删除无效 1403 面板重试、series/provenance、观测脚本及主机 C helper 测试修改；尚未提交。未修改 kShimC，未固化双启动，未写平板分区。所有产物路径均相对 `output/opd2413-stage4/`。

### 19.22 恢复测试结果：CTL 清退仍不足（2026-11-03）

- 恢复后的第 1、2 次通过（最终候选累计第 6、7 次）；第 3 次，即最终候选第 8 次失败。最终统计 **7/8 通过、1/8 失败**，未完成原计划 10 次。自动脚本首次失败即停止，已返回 Android；再次只读确认 `sys.boot_completed=1`、`_b`、boot_b 原 SHA-256 不变。本轮局部候选累计 **18/20 次已用，剩余 2 次**。
- 失败证据 `handoff-final/resume-runs/dmesg-03.txt`：1.323213 s 出现 SID `0x800`、IOVA `0xfc89bc00`、CB=8、FSR `0x402` 的 SMMU fault；1.523642 s DSI1 命令 DMA 超时，首条 DCS `ff 20` 返回 -110。两路 DSI IRQ 为 0/1，面板 prepare 未成功；ack 成功，Android 恢复与哈希复核通过。全部恢复轮记录在 `resume-runs/runs.jsonl`，总计数与最终设备核验见 `handoff-final/regression-summary.json`；当前无运行中的自动启动脚本。
- **结论修正**：默认 identity 匹配消除了早期 fault，但 INTF 停机加 6CTL 清理/复位仍不能保证之后不访问旧 framebuffer。此前 smmu3 3/3 和最终版前 7 次成功不足以证明根治；19.21 中的“最强证据支持 CTL”仅为当时假设，现已被反例限制。仅凭 SID 和落在旧 framebuffer 的 IOVA，尚不能唯一确认请求来自哪条硬件路径。
- 离线复核：当前 1406 只验证 INTF 状态与 CTL reset，不读取 VBIF outstanding/idle；因此不等价于总线请求已排空。现有 `dpu_hw_get_axi_halt_status()` 还会在 ACK 超时后因任一 AXI port idle 位置位返回成功，不能直接拿此返回值当作全部端口排空的证明。下一候选应先明确寄存器语义与真实请求来源。
- 下一步先只读取证：比较 SSPP/VBIF 未完成请求、DSI video/command 启用与 fault 时间，确认 CTL reset 完成后是否还需要其他明确的硬件完成条件。不得追加固定延时、面板重试或屏蔽 IOMMU fault 来宣称修复。
- 独立只读复核同意：SID 仅定位 MDSS，不能证明 CTL 来源。下一取证镜像应保存本轮首条 DMA 的实际 `DMA_BASE/LEN`，以及换域前后 SSPP 源地址、CTL active、VBIF 请求/确认状态；旧 diag1 的 DMA_BASE `0x5a000` 不能替代本轮快照。
- `1406` 与 `handoff-final`、`armada13` 均保留为诊断候选，**不标记稳定，不部署，不提交为已修复**。完整桌面与断电冷启动验证仍未做。

### 19.23 离线复盘：复位时 AXI 未 halt，旧读请求遗留在端口外侧（2026-11-03/04，候选 quiesce1 待上机）

只用已保存的 40 份 dmesg 和寄存器快照复盘（`stress1–5/broadcast1/reset1/vid1/diag1/clkxfer1/smmu1–3/handoff-final`），未上机。

- **1406 实际是空操作**：`msm_mdss_init()` → `msm_mdss_reset()` 在约 0.75 s 拉 `DISP_CC_MDSS_CORE_BCR` 20 ms，这时 ABL 的扫描输出还在读内存（DMA 域内核里每 8.33 ms 一次的早期 fault 证明 120 Hz 扫描一直跑到复位）。1406 在约 1.2 s 才执行，此时 INTF 早已停止：smmu2/smmu3/handoff-final 所有轮次都打印 `active mask=0x0`，CTL 也已被复位。1406 的 7/8 与基线（40 轮中 11 轮失败，约 27.5%）没有统计差异。
- **失败轮次的旧读请求位置固定**：凡是 SMMU fault 未被限流的失败轮次（5/5：broadcast1#2、clkxfer1#3、smmu1#1、smmu2#3、handoff-final resume#3），都在换域之后出现**恰好一次** SID 0x800 读 fault，IOVA 为 `0xfc89ba00`/`0xfc89bc00`/`0xfc89d800`。按 3392×4 B 行宽计算，都落在旧帧缓冲第 46/47 行交界、ABL 4 个 848 px rect 各自的起始 1 KiB 内（ABL 拓扑 4 LM/4 DSC/2 INTF）。12 次 identity 域通过轮次没有一次 fault。
- **fault 与首条 DCS 触发同时发生**：三次 fault→超时打印间隔为 200.43/200.75/200.82 ms，正好是 DSI 命令 DMA 等待（HZ=250）的长度；0.75 s 复位到首条 DCS 之间没有任何 MDSS 内存访问。diag1 失败轮次两路 `DMA_BASE` 都是正确的 `0x5a000`，fault 地址不是 DSI 自己的命令缓冲。
- **读通路整体卡死**：首条命令只有 DSI1 卡住，DSI0 完成；但之后的所有命令（重试/下一条 DCS）**两路都** `STATUS0=0x20003`。这是共享 AXI 读通路被堵，不是 DSI1 单独的问题。
- **同机已有同类结论**：erhai 内核 `c343837c7`/`2bb3a45c4`（本 tablet，同 SoC）记录 MDSS 在 VBIF AXI 端口未 halt 时掉电，之后首条 DSI 命令 DMA 永远取不到数据，症状逐字相同（两路 `CMD_MODE_DMA_BUSY`、FIFO 空、-110、黑到重启），且“用 BCR 复位整个 MDSS 也无效，坏状态在 AXI 端口外侧”。Armada 已带入其运行时挂起前的 AXI halt（`dpu_runtime_suspend()`，SM8750 `DPU_VBIF_AXI_HALT`），但**开机 `msm_mdss_reset()` 在扫描输出运行、AXI 未 halt 的情况下直接复位**，与掉电路径不对称。
- **结论（待实机确认）**：黑屏的触发点是开机复位打断正在进行的 ABL 扫描输出，在 MDSS 复位域之外留下一个孤立读请求。首条 DSI 命令 DMA 把它带出，它在 KMS 新页表中 fault，同时堵住 MDSS 读通路。第 47 行为何固定尚未解释（推测与帧消隐期的预取深度有关），不影响修复方向。

**候选 quiesce1（单变量，基于 handoff-final）**：仅在 `msm_mdss_reset()` 拉 BCR 之前、且 root 为 `oneplus,erhai`、MDSS 为 `qcom,sm8750-mdss` 时，按运行时掉电的量产顺序处理 ABL 管线。先停两路 DSI INTF（双缓冲，帧边界生效，等 `INTF_STATUS` 清零，上限 100 ms），再强制 VBIF 时钟、请求 AXI halt 并等 ACK（4 ms；无 ACK 时两个端口都 idle 才接受）。复位结束后再释放 halt。没有继承的扫描输出时不写任何寄存器；超时只报告，不阻止复位（与下游一致）。每次启动输出一行 `bootloader scanout: intf … axi halt … after reset …`，记录停机耗时、行/帧计数、ACK 和复位后的 halt 寄存器，因此通过轮也能提供数据。不加延时、重试或屏蔽 IOMMU fault；1406 及 identity 默认域保持不变。

- 离线：`msm_mdss.o` 以 `W=1` 编译无警告，`vmlinuz.efi` 编译 0 警告；主机寄存器模型 `linux/test_mdss_boot_handoff.py --kernel-source <源码>` 直接抽取内核函数，6 个场景通过（无继承扫描时零写入、两路先发停止再等待、AXI halt 只在停机等待之后且时钟已强制、超时正确传播、复位之后才释放）。两个变异反例（halt 提前、忽略 INTF 超时）都被测出。ESP 与 handoff-final 相比仅 `vmlinuz.efi` 不同（initramfs `4265cb5f…`、DT、loader、kShimC/U-Boot 均相同），4 份负载解包比对一致。
- 产物：`quiesce1/read-boot.img` SHA-256 `6f69c41d39d6dfafc5b567b30494596e52fd75d5811f112500e080b8c1c41d72`，kernel EFI `282ec507824e1f3493fefed1a7b4b6f73b0c6329e90d975c743c1354a9b0ca82`；源码差异 `quiesce1/candidate.diff`，修改前文件 `quiesce1/before-msm_mdss.c`。压力脚本新增 `boot_handoff` 字段（只解析 dmesg）。
- **上机判据**：每轮与以往相同（首次 prepare 成功、DSI IRQ 122/122、DCS/vblank 错误 0、SMMU fault 0），另外要求 `intf 0x3 stopped 0x3` 且 `axi halt ack`。连续 20 轮全部通过才算有效（基线失败率 27.5% 时，偶然连过 20 轮的概率约 0.16%）。首次失败即停止，并按失败轮的报告行判断：
  - `no-ack` 或停机不完整：排空没有完成，查端口为何一直忙。
  - ACK 正常但仍有第 47 行 fault：孤立请求早于 0.75 s，下一个嫌疑是 0.665 s SMMU 复位到 0.704 s identity attach 之间 SID 0x800 失配的窗口。
  - 无 fault 但仍卡住：属于另一种机制。
- 需要用户重新授权临时启动次数（上一轮授权剩余 2 次，不够做统计）。仍只用 `fastboot boot`，不刷写、不切槽。

### 19.24 quiesce1 第 1 轮失败；改为先复位 CTL 再 halt（quiesce2，2026-11-04）

- 用户授权本轮 20 次 `fastboot boot`（首次失败即停）。开始前 b 槽、boot_b `0e5afd65…`、电量 100%、29.6 °C。**quiesce1 第 1 轮即失败**，自动回到 Android，复核槽位/哈希不变。本轮已用 1/20。证据 `quiesce1/runs/`、`quiesce1/stress-host.log`。
- 报告行：`intf 0x3 stopped 0x3 in 656 us (cfg 80800000/80800000 line 2458/2460 frame 6993/6993), axi halt no-ack 00000030 in 4011 us, after reset 00000000/00000000`。随后 1.495 s 出现 SID 0x800 读 fault，IOVA `0xfc89d800`（第 47 行，与 clkxfer1#3 相同）；1.696 s DSI1 超时，首条 `ff 20` 返回 -110。
- 解读：
  - INTF 在帧边界停下（656 µs，约 209 行，正好到帧尾）。`INTF_CONFIG` bit31 表明 ABL 开启了 programmable fetch，停下时下一帧的预取已经开始。
  - AXI halt **一直没有 ACK**，只有两个端口显示 idle。quiesce1 采用了 erhai 的回退判定（端口 idle 即算成功），因此没有报警。
  - 复位后 VBIF halt 寄存器为 0，说明 VBIF 寄存器被 BCR 复位了，但第 47 行的请求仍然存活。
  - 在帧边界停 INTF 会**确定性地**把 SSPP 留在"下一帧已预取、第 47 行请求挂起"的状态。quiesce1 很可能把约 27% 的概率性故障变成了必然故障。
- 主线与下游关显示的标准顺序（`dpu_encoder_helper_phys_cleanup()`）是：INTF 停下后，**趁 CTL 仍拥有这些 pipe 时先 `ctl->ops.reset()`**，再清 mixer 等。1406 把顺序做反了（先清关联再复位），而且是在 BCR 之后；erhai 的掉电 halt 能拿到 ACK，是因为 DRM 之前已经做过这一步。
- **quiesce2（单变量，相对 quiesce1）**：停 INTF → 复位 ABL 拥有扫描输出的 CTL（看 `CTL_INTF/FETCH_PIPE/PIPE_ACTIVE`，都没有时复位全部 6 个；软复位 2 ms 未完成再走 override，与 `dpu_hw_ctl_reset_control()` 一致）→ AXI halt，**必须 ACK**，不再接受 idle 回退 → BCR → 释放。报告行增加 prog fetch 起点、CTL 掩码、复位方式和耗时、复位前后 SSPP 是否残留（`SRC_SIZE/SRC0_ADDR`），用来判断 BCR 是否会清掉 pipe 状态。
- 离线：`W=1` 无警告，`vmlinuz.efi` 0 警告；寄存器模型扩展到 9 个场景并全部通过（含 override、override 失败、无 CTL 认领时全部复位、ACK 严格要求、SSPP 残留记录）；三个变异反例（CTL 复位提前、halt 提前、退回 idle 回退）都被测出。ESP 与 handoff-final 相比仍只有内核不同。镜像 `quiesce2/read-boot.img` `2686192103ed5a1b03e3903d0627967e2c0dcec8f490d2d93e157a9c8b4d587b`，kernel `77e4a3c4c7438db90689dc6ad1b9bc782323e81eb29de491e98b44a90696f5be`，差异 `quiesce2/candidate.diff`。
- 判据：每轮在原有判据之外，要求 `ctl … reset soft|forced` 且 `axi halt ack`。连续通过才算有效，首次失败即停，并按报告行分流：
  - ACK 仍拿不到：挂起的请求不在 CTL 的复位范围内。
  - ACK 正常但仍有 fault：请求在 AXI 端口之外，嫌疑转向 0.665–0.704 s 的 SMMU 失配窗口。
  - 复位后 SSPP 仍残留：说明 BCR 不复位 pipe，需要另行清除。
- **quiesce2 第 1 轮内核崩溃**（本轮授权已用 2/20）：探针没有应答，30 s 后 panic 重启回到 Android，槽位/哈希不变。Android pstore 里留下 Linux `console-ramoops-0`（`quiesce2/runs/pstore-01-console-ramoops-0`，有已知的位损坏），在 **0.783 s** 停于 `Internal error: synchronous external abort`（ESR 约 `0x96000010`，读访问）。按 quiesce1 的时序推算，复位前的步骤约在 0.758 s 前结束，0.78 s 前后撤销复位，因此 abort 落在复位刚结束后的 `mdss_boot_release_hw()` 里。那里比 quiesce1 只多了一项：复位后读 SSPP 寄存器（quiesce1 复位后只读 VBIF，没有问题）。如果 abort 发生在复位前的 SSPP 扫描或 CTL 复位，时间会早 25 ms 以上。**结论：BCR 撤销后，MDP 子模块不能立即访问，VBIF 可以。** 复位后的 SSPP 残留诊断因此改不了，不能这样做。
- **quiesce2b**：只删除复位后的 SSPP 读取，停机、CTL 复位和 ACK 判据不变。寄存器模型新增约束"复位后不得访问 MDP"，这个约束能抓到 quiesce2 的这个 bug，quiesce2b 通过。镜像 `quiesce2b/read-boot.img` `571c06cdb6b90a2567a60f365e2ea886be73de1e6cbb3020dd5dce8ac4212e55`，kernel `cb3da8b8d3bf7d21c7aed19110255ed6d270e3d303f6aa1f41d254f8fe5a518c`，相对 quiesce2 的差异 `quiesce2b/delta-from-quiesce2.diff`（53 行）。使用本轮剩余的 18 次授权，判据同上。

### 19.25 quiesce2b：AXI ACK 与显示结果逐轮对应；改为先关 programmable fetch（quiesce3，2026-11-04）

- quiesce2b 第 1 轮**通过**：`ctl 0x1 reset soft in 3 us, axi halt ack 00000031 in 1 us`，面板首次 prepare 成功，DSI IRQ 122/122，没有 SMMU fault。第 2 轮**失败**：CTL_0 软复位同样 3 µs 完成，但 `axi halt no-ack 00000030 in 4005 us`，1.483 s 出现第 47 行 fault（`0xfc89be00`），DCS 超时。脚本停止并回到 Android，槽位/哈希不变。本轮授权已用 4/20。证据 `quiesce2b/runs/`。
- 报告行确认 ABL 使用 **CTL_0 和 VIG0–3** 四个 pipe（`sspp 0xf`，VIG0 `SRC0=0xfc800000`）。`PROG_FETCH_START=0x1c9b09` 按 `(vtotal−L)×htotal+1` 解出 htotal=712、L=25，即帧尾前 25 行开始预取下一帧。
- 目前 3/3 逐轮对应：AXI halt 有 ACK 就通过，没有 ACK 就失败（quiesce1#1 无 ACK、quiesce2b#1 有 ACK、quiesce2b#2 无 ACK）。**能否排空是决定因素**；CTL 软复位只是有时能撤掉挂起的预取。#1 检测到 INTF 停止约在帧尾后 60 行，#2 约在 146 行，推测预取仍在进行时复位能干净中止，等预取填满、请求挂起后再复位就撤不掉了（推测，未证实）。
- **quiesce3（单变量，相对 quiesce2b）**：在停 timing engine 之前，先清两路 `INTF_CONFIG` bit31（programmable fetch），通过认领扫描输出的 CTL 写 `CTL_INTF_FLUSH=BIT(1)|BIT(2)` 和 `CTL_FLUSH=BIT(31)`（与 encoder enable 下发 INTF 配置的方式相同），等 `CTL_FLUSH` 被下一帧消费（上限 100 ms）。新帧不再预取下一帧之后，才停 timing engine → CTL 复位 → AXI halt 并要求 ACK → BCR。没有 CTL 认领时不 flush，flush 超时会上报。报告行增加 `prog fetch off <mask> in <us>`。预期：每轮都能 ACK。
- 离线：`W=1` 无警告，`vmlinuz.efi` 0 警告；模型扩展到 10 个场景并全部通过（只清 bit31、只通过认领的 CTL flush、flush 被消费后才停、prog fetch 已关时不 flush、无 CTL 认领时不 flush、flush 超时上报）；三个新变异反例都被测出。ESP 与 handoff-final 相比只有内核不同。镜像 `quiesce3/read-boot.img` `4554e8c121cbf6a08f125235ba875bf17363a9b4e89ba78075126c507b1fa123`，kernel `aff4d7823140101e4269ce07aa0b3f88fd109f472e1467b76870a836e003ab66`，相对 quiesce2b 的差异 `quiesce3/delta-from-quiesce2b.diff`。

### 19.26 quiesce3 16/16 通过：开机黑屏根因确认并修复（2026-11-04）

- 用户同意用本轮剩余 16 次测试 quiesce3。**16/16 全部通过**：每轮面板首次 prepare 成功，DSI IRQ 122/122，DCS/vblank 错误 0，SMMU fault 0（完整 dmesg 含用户态标记）。每轮报告行都是 `prog fetch off 0x3 … stopped 0x3 in ≈8.4 ms, ctl 0x1 reset soft in 2–3 us, axi halt ack 00000031 in 0–1 us`。开始时的扫描行覆盖 284–2635，即一帧中的各个位置。第 5 轮起始于 2635 行，已越过预取起点，flush 等了一整帧才生效，之后同样拿到 ACK。本轮授权 20/20 已用完；每轮都回到 Android，最后复核 `_b`、unlocked、boot_b `0e5afd65…`、电量 100%。证据 `quiesce3/runs/`（`runs.jsonl`、16 份 dmesg/display）。
- 统计：基线失败率 11/40，偶然连续通过 16 轮的概率约 0.58%。本会话所有带诊断的 19 轮里，AXI ACK 与结果一一对应：有 ACK 的 17 轮全部通过，无 ACK 的 2 轮全部黑屏。
- **根因（已确认）**：ABL 交接时，CTL_0 与 VIG0–3 仍在扫描输出，并开启了 programmable fetch（帧尾前 25 行开始预取下一帧）。`msm_mdss_init()` 直接拉 MDSS core BCR，pipe 已经发出的下一帧预取（第 46/47 行交界）残留在复位之外。KMS 换成新页表后，第一次 MDSS 总线事务（首条 DSI 命令 DMA）把它放出，它 fault 后堵住 MDSS 读通路，两路 DSI DMA 永久 busy，导致黑屏。只要复位前 VBIF AXI 能拿到 ACK（说明总线已排空），就不会出现这个问题。
- **修复（1407）**：BCR 之前依次：通过 CTL INTF flush 关掉 programmable fetch，等新帧生效 → 停 DSI timing engine → CTL 复位 → VBIF AXI halt 并要求 ACK → BCR；复位后只碰 VBIF 释放 halt。补丁已放入 series（`1407-drm-msm-mdss-retire-opd2413-boot-scanout-before-reset.patch`，紧接 1406），PATCHES.md 已登记，1406 的说明也已更正。干净的 7.2.6 按 series 应用 300 个补丁全部零 fuzz，hunk 数逐个核对；`drivers/gpu/drm/msm`、`arm-smmu`、`panel` 与实测源码逐字节一致。checkpatch 仅剩日志参数宏一条误报。寄存器模型 `linux/test_mdss_boot_handoff.py` 10 个场景通过，8 个变异反例都被测出。仍未提交。
- 1406 中 DPU 侧的 INTF 停机和 CTL 清理在复位之后执行，实际上什么也不做，只是因为与实测配置一致才保留。identity 默认域条目是必要的。以后如果要精简 1406，需要另做回归。

### 19.27 完整系统验证：armada14 模块不配套失败，armada15 桌面通过（2026-11-04）

- 代码已提交：`98f8101`（1406/1407 补丁、series、PATCHES.md、两套寄存器模型、压力脚本）、`0184dba`（本文档）。用户授权完整系统各测 1 次桌面和游戏模式。
- **armada14 失败（与显示无关）**：在 armada13 的基础上只把 ESP 内核换成 quiesce3 内核。U 盘镜像校验通过（约 200 s）后，`insmod /opd/overlay.ko` 报 `failed to validate module [overlay] BTF: -22`，随后自动回到 Android，槽位/哈希不变。原因：`CONFIG_DEBUG_INFO_BTF_MODULES=y`，每次重编 vmlinux，旧模块的 BTF 都对不上。armada10–12 用的一直是第五轮内核 `c8614378…`，与 U 盘里那套模块属于同一次构建；之后重编的内核只跑过不加载模块的冒烟 initramfs，所以问题一直没暴露。armada13 当时只做了离线比对，同样有这个问题。**教训：换内核必须连同模块一起重编并部署，不能只换 `vmlinuz.efi`。**
- **armada15**：在同一棵源码树上一次编出 `vmlinuz.efi`（`015e5b2f…`，代码与实测的 `aff4d782…` 相同，只是构建时间戳不同）和 1594 个模块（`INSTALL_MOD_STRIP=1`，模块清单与旧内核包、U 盘镜像完全一致）。initramfs 只替换 `opd/overlay.ko`（直接改写 newc 流，134 个条目不变，`181755df…`）。新模块树经用户同意通过 pkexec 写入 U 盘可写层 `opd/upper/usr/lib/modules/7.2.6`：1610 个文件逐个校验，清单存为 `opd/modules-overlay.sha256`；只读的 `root.squashfs` 不改动；可写层原先没有 `usr/lib/modules`，删掉这个目录即可恢复。镜像 `armada15/read-boot.img` `af5ef0c6becc19b477d27bb95a04f17d2f688ed442bda92efaee19490b00b167`，暂存的模块树在 `armada15/stage/`。U 盘镜像里的 `initramfs.img`（dracut，正式 ostree 启动用）仍是旧模块，U 盘启动路径用不到，以后重新出镜像时需要一并更新。
- **armada15 桌面模式通过（用户实测）**：内核接管后正常显示并进入桌面，单指/多指触屏、磁吸键盘、键盘触控板及其多指手势都正常。
- U 盘每次启动都要完整 SHA-256 校验约 3.4 分钟（USB 2.0 下读 6.7 GB）。用户觉得太慢，计划改成"首次完整校验、之后比对记录"，正式安装时再换成 dm-verity。
- SSH 只读取证（armada15，运行约 650 s）：报告行 `prog fetch off 0x3 … stopped 0x3 in 8466 us, ctl 0x1 reset soft in 3 us, axi halt ack 00000031 in 1 us`，紧接着 `panel prepared`；两路 `dsi_isr` 都是 122；没有 SMMU fault 和 DCS 失败；加载了 92 个模块；`degraded` 只来自已知的 `armada-controller-type`。304 s 出现两次 `vblank timeout`（相隔 60 ms），前后没有用户态事件，画面一直正常，与第八轮 830 s 的现象相同，属于"超时告警但画面正常"一类，暂不追。日志在 `armada15/live-{dmesg,journal-k,journal}.txt`。注意：这个启动项没有 `log_buf_len`，dmesg 会覆盖开机阶段，要用 `journalctl -k -b`。

### 19.28 校验改为首次完整校验；游戏模式通过；音频缺拓扑（2026-11-04）

- `smoke_initramfs.py --usbroot`：完整校验通过后写 `opd/root.verified`（`v1 size inode mtime ctime sha256`）。之后只比对这份记录；镜像被替换或改写会改变 inode/ctime，记录随之失效；`opd.verify=full` 强制完整校验；校验失败时删除记录。在主机 `sh` 上测了 6 个场景，全部符合预期（initramfs 里 busybox 1.36.1 的 `grep -w`、`stat %i/%Y/%Z` 已确认支持；它在 binfmt/qemu 下无法测 standalone 小程序，所以没用它跑控制逻辑）。initramfs 的处理方式：先用旧模板加原参数逐字节重现 `/init`，再只替换校验段。**armada16**（`909c3d96…`）= armada15 + 新 initramfs（`0a61661d…`），内核和模块不变。
- armada16 实机：第一次完整校验并写入记录；第二次启动 `full check skipped`，换上 U 盘后几秒就继续启动。
- **游戏模式通过（用户实测）**：第一次停在 Steam 加载界面，原因是直连 Steam 超时（`store.steampowered.com` 直连超时，走用户局域网 HTTP 代理返回 200）。代理写在 `/etc/environment.d/90-opd-proxy.conf`（只在 U 盘可写层；Steam 跑在 systemd 用户服务里，从 environment.d 继承变量，`no_proxy` 排除本机和局域网）。重启后 Steam 先安装上次已下载的客户端更新（启动画面显示 "Still starting…"，约 3 分钟），随后进入界面，触屏、键盘、触控板都正常。
- **音频：没有声卡**。ADSP 在运行，q6apm/q6prm、LPASS 宏、WCD939x、6 颗 AW88261、sc8280xp 声卡驱动和 SoundWire 都已加载，但 `qcom-apm: tplg firmware loading qcom/sm8750/OnePlus-Pad-2-Pro-tplg.bin failed -2`，于是 `snd-sc8280xp: failed to instantiate card -2`。DT 声卡链路：扬声器 CPU DAI 40、WCD 放音 113、WCD 录音 120、DP 104。KONKR 拓扑里的 WCD/DP 段可以复用；扬声器端口不同（KONKR 用 `SECONDARY_MI2S_RX`），需要为这台平板编写 `audio_topology/sm8750/SM8750-OnePlus-Pad-2-Pro.m4` 并生成拓扑文件，属于第 G 项音频移植。

### 19.29 G 音频移植（进行中，2026-11-04）

- **输入来源**：本机 `~/github/onepluspad-2pro-mainline`（erhai 的用户态打包仓库）提供了：
  - 拓扑源码 `packaging/audio/OnePlus-Pad-2-Pro.m4`，以及自定义宏 `module_tdm.m4`、`module_mfc_cfg.m4`；
  - 固定版本的 audioreach-topology 源码包 `993a17dc`，SHA-256 `58e0fa0a…` 已核对，BSD-3-Clause；
  - UCM `ucm2/{conf.d,Qualcomm}/sm8750/OnePlus-Pad-2-Pro*` 和 `erhai-audio-speaker` 脚本。
- **拓扑**：在一次性的 Fedora 44 容器里生成（`alsa-topology-utils` 1.2.16、m4 1.4.21，不改主机）：`m4 -I packaging/audio -I <audioreach-topology> OnePlus-Pad-2-Pro.m4` → `alsatplg`，得到 `OnePlus-Pad-2-Pro-tplg.bin`（17292 字节，`d2fd4668…`）。三条后端未定义的告警是预期的。erhai `build-debs.py` 的 `check_topology`/`check_module_configs` 校验通过；扬声器 MFC 的 2→8 映射为 `L R L R L R L R`。产物在 `audio1/tplg/`。
- **功放固件**：用 payload-dumper-go 从本机当前 OTA（16.0.10.600 CN01）只读解出 odm（EROFS），在容器里用 `dump.erofs` 取出 `firmware/aw882xx_acf.bin`（4976 字节，`97ae5a34…`），与 erhai 清单一致，安装为 `awinic/erhai/aw88261_acf.bin`。是 ROM 内容，不是单机校准数据。
- **内核（未提交，候选 audio1）**：Armada 的 audioreach 完全不支持 TDM。erhai 的 TDM 支持混在提交 `e5493a910` 里（标题写的是面板参数，实际是大合并），只能按文件、以 erhai 相对 v7.2.14 的差异移植：
  - 能干净应用的：`snd_ar_tokens.h`、`audioreach.h`、`topology.c`、`q6apm-dai.c`、`common.c/h`（TDM 槽解析、位时钟反相、`qcom_snd_be_hw_params_fixup`）、`qcom/Kconfig`（select `SND_SIMPLE_CARD_UTILS`）、`aw88261.c`。
  - 三方合并，冲突手工解决：
    - `audioreach.c`：保留 Armada 0007 的声道映射，加上 erhai 的 `bits_per_sample`；
    - `q6apm-lpass-dais.c`：保留 Armada 的挂起/恢复，加上 erhai 的 `q6tdm_ops`，取时钟处加 TDM `case`；
    - `q6prm.h`、`q6prm-clocks.c`：MCLK 和 TDM 的时钟 ID 都保留。
  - `sc8280xp.c` 不采用 erhai 的整体重构：
    - 只在 hw_params 开头加 `PRIMARY_TDM_RX_0…QUINARY_TDM_TX_7` 分支，交给移植来的 `sc8280xp_tdm_hw_params`（设 CPU 位时钟，与 erhai 的 sm8750 配置一致）；
    - BE fixup 末尾调用 `qcom_snd_be_hw_params_fixup` 读取 `convert-*`。
    - 只有 OPD2413 的 DTS 用到 TDM 和 `convert-*`，其他机型的执行路径不变。
  - 暂不移植：
    - erhai 的 `wcd939x.c` 改动（按声道记端口开关的修正，KONKR 也在用这个驱动）；
    - lpass 宏约 630 行的 PM 重构；
    - `soundwire/qcom.c`。
  - `q6dsp-lpass-ports.h` 和 `codecs/Kconfig` 已经与 erhai 一致。
  - W=1 无警告，模块 0 警告；修改前的文件存于 `audio1/before/`（与 clean-series 逐字节一致）。
- **构建注意**：在共享源码树上跑过 `W=1` 后，vdso 会重新编译，导致 vmlinux 重新链接，于是这次的内核变成 `2fc33641…`，1594 个模块也重新生成。源码与之前相同（`overlay.ko` 逐字节一致，说明 BTF 没变），但仍要作为一套整体部署。以后不要在这棵树上用 W=1，或改用独立的 O= 目录。
- **armada17**（`3aa77401…`）= armada16 + 内核 `2fc33641` + 配套 initramfs。新模块、拓扑、功放固件、UCM、`erhai-audio-speaker` 和 `/etc/systemd/sleep.conf.d/90-opd-no-suspend.conf` 都写入了 U 盘可写层（1618 个文件逐个校验，清单 `opd/audio1.sha256`）。
- **挂起问题**：armada16 自动挂起后死机，无法唤醒（第 H 项，挂起/恢复尚未验证），用户只能拔 U 盘，之后 `e2fsck` 回放日志、清理了两个孤立 inode。测试系统已禁止挂起和休眠。
- **armada17 第 1 次（音频授权 1/3）失败，是我的部署错误**：Fedora 44 的 `/usr/sbin` 是指向 `bin` 的符号链接，安装脚本在可写层建了真实的 `usr/sbin` 目录（只放了 speaker 脚本），把整个 `/usr/sbin` 遮住了。结果 modprobe（retcode 127）、wpa_supplicant（EXEC ENOENT）、zram、binfmt 等 12 个单元失败，Wi-Fi 显示红叉，声卡模块也没能加载（只加载了 56 个模块）。日志在 `armada17/a17-{dmesg,journalctl}.txt`。修复：删除可写层的 `usr/sbin`，脚本改放 `/usr/bin`。部署流程补上了用只读镜像（`unsquashfs -lls`）核对目标路径类型的检查，已用 `usr/sbin` 做反例验证。**教训：往 overlay 可写层离线写文件时，必须对照下层的合并视图检查路径类型，不能只看可写层。**
- 当前：armada17 第 2 次已发送（音频授权 2/3），等待用户进入桌面后用 SSH 检查声卡。扬声器按 erhai 的设置保持默认关闭，用户同意低音量试听。

### 19.30 桌面无输入/输出设备：UCM 修复及播放验证（2026-11-04）

- **场景与版本**：armada17 第 2 次，Wi-Fi SSH 调试，USB 持久 overlay；没有重启、更换内核或写 Android 分区。Armada 工作树基线 `2e18cf21`，boot SHA-256 `3aa77401ad784114a4dfe9f679aa1378882610bc09642c5f6cc4cbd089a946b8`，内核 `2fc3364136c18613d4543f19ce715889cb6e3f721aa4167fe1a1637539e877c7`，7.2.6 #29；ALSA 1.2.16.1、PipeWire 1.6.11、WirePlumber 0.5.19。所有本轮原始证据在 `output/opd2413-stage4/audio2/`。
- **基线**：声卡、三个 PCM、六颗 AW88261 及 ACF 均已加载，故 §19.28 的缺拓扑已解决。桌面只有 Dummy Output、声卡 profile 为 off；`snd_use_case_mgr_open("hw:0")=-2`（`baseline.txt`）。
- **四个配置问题与修复**：
  1. ALSA 查找 `CardLongName`，实际为 `armada-OnePlusPad2ProU_Bootobserver`，已部署文件却只有 `OnePlus-Pad-2-Pro.conf`。加当前长名称到原配置的符号链接后，UCM open 和 HiFi verb 均返回 0。查找规则以设备上的 `/usr/share/alsa/ucm2/ucm.conf` 为证，也核对了 [ALSA 官方 UCM 文档](https://www.alsa-project.org/alsa-doc/alsa-lib/group__ucm__conf.html)。
  2. erhai 的 UCM 引用当前 Armada 内核没有的 `TX0/1 HPF cut off`，Mic enable 返回 -2。两项改用 `ControlExists` 条件；必要的路由控件仍严格执行，避免掩盖缺失。
  3. 扬声器被 `/run/erhai-audio/speaker-enabled` 临时标志控制，重启后消失。改为 HiFi 始终包含 Speaker；六颗功放从原配置 350 改为保守的 270（-22.5 dB），DSP gain 保持 8192（Q13 unity），桌面使用软件音量。
  4. 扬声器 FE→BE 路由原来在 Speaker EnableSequence；PipeWire 在 enable device 前探测 PCM，open 返回 EINVAL，于是丢弃整个 HiFi profile。把该路由与已有麦克风 FE 路由一起放到 SectionVerb，HiFi 随即 supported（`wp-debug.txt`、`pipewire-route.txt`）。
- **源码与部署**：新增 `system_files/usr/share/alsa/ucm2/Qualcomm/sm8750/OnePlus-Pad-2-Pro/{HiFi,Speaker}.conf` 及 `conf.d/sm8750/{OnePlus-Pad-2-Pro,armada-OnePlusPad2ProU_Bootobserver}.conf`，由现有 `40-vendor-system-files.sh` 自动安装。来源为本机 erhai 用户态仓 `9762c0994e2b5acd672be2ddbda680fd61f1cad6` 的 BSD-3-Clause UCM，删除不适用于本次验证的历史音量断言。源文件与设备文件 SHA-256 一致（`final-check.txt`）：主配置 `f7cecda0…`、HiFi `d2b937ad…`、Speaker `8afb84af…`。修改前设备配置备份在 `/var/lib/armada/audio2-backup/OnePlus-Pad-2-Pro/`。未提交。
- **机器验证**：UCM Mic/Speaker enable、disable 均返回 0；ALSA 48 kHz/S16_LE/双声道全零播放 3 秒返回 0（`silent-test2.txt`）。真实 PipeWire/Pulse 路径三次 paplay 返回 0，PCM 为 RUNNING、MMAP_INTERLEAVED，硬件指针推进；内置麦克风录到 997 Hz 测试信号，40%→70% 音量时两通道的单频幅度约增加 5 倍（`loopback-pa.txt`、`recordings/analysis.json`）。静音录音夹有环境/瞬态峰值，不能据它声称静音信号为零；静音判定采用用户实际听感。第一次脚本使用本机未安装的 pw-play/pw-record，返回 127；已改用系统已有的 paplay/parec，保留失败记录。
- **用户验收：真机通过**。用户明确确认“能出声，音量和静音均正常”，对应桌面网页视频。尚未逐颗验证六个扬声器，麦克风仅有测试音采集证据，语音增益/音质没有完整验收。
- **持久性检查**：删除 `/run` 标志，关闭 WirePlumber，将两条 FE 路由清零，再启动 WirePlumber，自动恢复 HiFi、Speakers、Internal microphones；六个 Amp volume 仍为 270，DSP 为 8192（`final-check.txt`）。已关闭临时调试日志并 sync。此检查排除了临时标志及已开通 FE 路由的依赖，**不等同冷启动验证**。
- **日志边界**：本轮修复/播放期间没有新增 APM、AW88261 或 ASoC 错误；本次启动早在 10:20:03 就有一次 `qcom-apm CMD timeout [1001021]`，以及 WCD 缺 mbhc buttons 属性警告。因此只关闭“桌面无声音设备、网页无声”问题，不宣称整个启动音频链路零错误。证据 `final-journal.txt`。
- **当前设备与交接**：平板仍在 Armada 桌面，Wi-Fi 在线，默认扬声器音量 40%、未静音；没有遗留录音/播放测试进程。音频测试资料在主机忽略目录和设备 `/tmp/opd-audio2`。唯一下一步是冷启动音频回归；本轮没有占用新的临时启动次数。原来的 `erhai-audio-speaker` 开关脚本已被始终启用的 UCM 取代，不再用于控制扬声器。

#### 音频临时处理 / 技术债（统一登记入口）

| 项目 | 位置、原因与影响 | 去除条件 |
|---|---|---|
| 当前 U-Boot/DMI 长名称别名（§19.31 已解决） | `conf.d/sm8750/armada-OnePlusPad2ProU_Bootobserver.conf`；精确匹配本轮启动链，其他 SM8750 设备不受影响，但更换 DMI 后可能失配 | 已改为按 CardName 窄范围匹配，源码及平板旧别名均已移除；新镜像冷启动另验 |
| 扬声器保护及每路调音 | 用户决定按参考项目采用六路350/-2.5 dB（§19.38），DSP保持8192；当前没有原厂 DSP 扬声器保护和每路调音 | 独立移植和验证保护/调音；350与参考项目一致，不代表已经恢复原厂完整音频链路 |
| 已部署候选未进入完整构建（§19.31 源码已解决） | §19.29 audio1 内核改动、拓扑和私有 ACF 仍在候选/USB 层；完整重建还不能复现当前音频能力 | 内核补丁、拓扑源码/生成器、固件输入和构建检查已纳入；完整镜像/冷启动仍为验收待办 |

### 19.31 音频源码与镜像构建集成（2026-11-04）

- 用户明确要求“从源码层面修复”。路线和实验判据在 `output/opd2413-stage4/audio-source/PLAN.md`；本轮不更换正在运行的内核，不重启或修改 Android。
- **内核**：新增 `1408-ASoC-qcom-add-opd2413-tdm-audio.patch` 并加入 `packages/kernel/patches/series`，PATCHES.md 记录完整来源 `e5493a9107c16b53c58f477b7619ed511d4fe68e`。13 个修改前备份全部与原 clean-series 源码一致，补丁 55 个 hunk 以 `-F0` 应用后，13 个文件逐字节等于已实测的 audio1 源码。证据 `patch-replay.log`、`source-equivalence.json`。没有引入额外内核行为，也没有为了本轮重新链接运行中的内核。内核包内容哈希已改变，旧缓存不再匹配；构建继续将内核及模块作为同一套打包。
- **拓扑**：新增 `audio_topology/oneplus-pad-2-pro/`，包含板级 m4、两份扩展宏、BSD-3-Clause 许可证、固定上游 `993a17dcb672357998a463a73f120064d6c74f4f` 及归档/产物哈希、生成器和构建说明。实测 blob 进入 `system_files/usr/lib/firmware/qcom/sm8750/OnePlus-Pad-2-Pro-tplg.bin`。Fedora 44 隔离容器重新生成 SHA-256 `d2fd46689b0232263380984cf4a23b5261a98ed68e5695b1be96a8c25f47a107`，与实机产物完全一致；三条内核提供的 BE widget 告警保持预期。初次容器因 SELinux 拒绝输出目录写入，改为仅该临时容器 `label=disable` 后成功，没有修改主机目录标签。
- **UCM 长名称根治**：新增 `conf.d/sm8750/sm8750.conf` 驱动 fallback，仅在 `${CardName} == OnePlus-Pad-2-Pro` 时包含板级配置。删除上一轮的 DMI 长名称别名，不再绑定 `U_Bootobserver`。平板移除旧别名后 `snd_use_case_mgr_open(hw:0)=0`、`_file=sm8750.conf`；重启 WirePlumber 后 Speakers/Mic 保持正常（`stable-ucm.txt`）。新配置没有 `/run` 开关依赖，功放增益保持 270、DSP unity。
- **私有固件和镜像**：`prepare_audio_firmware.py` 先核对所有输入，再向 Git 忽略目录复制 60 份固件；哈希清单保存在 `system_files/usr/share/armada/firmware/`，不含原始镜像路径/设备身份数据。新 `41-check-oneplus-audio.sh` 接入系统构建，在 `ARMADA_DEVICE=oneplus-pad-2-pro` 时强制检查固件、拓扑和 UCM。未显式选择 OnePlus 目标而存在任一私有目录时失败，防止本地残留输入混入普通镜像。Containerfile 与 Justfile 传递该参数；本机构建输入已准备。完整命令见 `audio_topology/oneplus-pad-2-pro/README.md`。
- **initramfs**：`55-generate-initramfs.sh` 对显式 OnePlus 目标，将清单全部固件和拓扑加入 dracut `--install`，并延续既有 `lsinitrd` 精确路径检查。完整构建仍按 step 20 安装同套内核/模块后才生成 initramfs。临时 Armada 容器已用 audio1 配套内核/模块完成该步骤，61 个 OnePlus 固件/拓扑文件均出现在真实归档中，产物 84388227 字节、SHA-256 `ff408d81bb38f3c44419389e4e00bce0816166ab7deb475705ad8a6adf15afd9`。证据 `initramfs-build.log`、`rebuilt-initramfs.list`、`initramfs-validation.json`；没有部署到平板或覆盖现有 U 盘。
- **构建检查**：`tests/oneplus-audio-image-test.sh` 已通过：完整输入成功；目标缺全部固件、缺 ACF、损坏 ACF、缺稳定 UCM 均失败；仅残留 ADSP 或仅残留 ACF 的通用镜像均失败。准备工具的损坏输入负对照确认不会写出一部分目标文件。shell 语法和 Python 编译检查通过，独立审查未发现剩余阻塞。私有目录 `git check-ignore` 通过、没有已跟踪文件。
- **当前状态与边界**：源码集成已完成，本轮提交范围见 §19.33；平板仍在同一次 Armada 桌面运行，Wi-Fi 在线，重启声音服务后保留用户当前音量 15%。唯一下一步是完整新镜像、全新可写层及冷启动音频验收；没有把源代码/构建检查通过当作安装镜像真机通过。

### 19.32 Konsole 提示 armada-controller-type.service 失败（2026-11-04）

- **基线**：同一次 armada17 / 7.2.6 #29 / InputPlumber 0.83.0；InputPlumber 正常运行，但 `devices list` 和 D-Bus 均没有 CompositeDevice。平板没有内置游戏手柄，键盘/触控板走各自输入路径。原 `controller-type apply` 固定重试 30 次仍没有手柄，于 10:20:18 返回 1，系统一直 degraded，打开 Konsole 时显示 failed unit。证据 `output/opd2413-stage4/controller1/baseline.txt`。
- **源码修复**：新增设备能力 `ARMADA_HAS_BUILTIN_GAMEPAD`，defaults=1，OnePlus profile=0，经 `device-env` 导出。`controller-type` 读取一次设备环境，只有自动 `apply` 且该值明确为 0 时记录原因并成功跳过；显式 `get/set` 保留。没有配置、环境读取失败及其他掌机仍执行原应用/错误路径，不会掩盖真正的手柄故障。文件位于 `system_files`，既有镜像复制流程会安装，无运行时临时开关。
- **测试**：新增 `tests/controller-type-test.sh`，通过平板两种 model 无等待/无保存副作用、掌机成功与缺手柄失败、平板显式设置外接手柄、旧环境兼容等检查；原 `input-calibration-test.sh` 和 `perf-settings-test.sh` 通过。独立审查无阻塞。计划和证据在 `controller1/`。
- **部署与真机结果**：同步 4 个源码文件，重启此 oneshot 服务；11:30:08 返回 `status=0/SUCCESS`、`active (exited)`，日志明确 `Skipping controller type: this device has no built-in gamepad`。`systemctl --failed` 为 0，`systemctl is-system-running` 为 `running`；InputPlumber 仍 active（`live-fixed.txt`）。当前 Konsole 的新会话不应再报告该失败项；历史终端输出本身不会被改写。
- **交接**：平板仍在 Armada 桌面、Wi-Fi 可用。服务未禁用/屏蔽；本轮未重启系统，后续完整新镜像冷启动时同时复核此服务。当前唯一下一步仍为新镜像及全新可写层音频/启动回归。源码修改纳入 §19.33 本轮提交。

### 19.33 本轮提交与最新交接（2026-11-04）

- 按用户“更新文档，然后提交”的最新指令，本次提交包含：§19.29–19.31 的音频 1408 内核补丁/series/provenance、拓扑源码/生成器/生成物、UCM、私有固件哈希/准备工具、镜像目标及检查、initramfs 收录，§19.32 的无内置手柄能力与服务修复，以及相关测试和构建说明。此前“不提交本文档”的限制在本轮由此明确指令替代；设备标识、原始日志、私有固件及无关的对话导出仍不纳入提交。
- 验证结果：13 文件/55 hunk 还原实测音频源码；固定上游拓扑重建 SHA-256 一致；实际 dracut 归档包含全部 61 份 OnePlus 固件/拓扑；稳定声卡名称 UCM 真机识别；用户确认网页音频、音量和静音；手柄服务真机返回 0、failed units=0、系统 running。新增音频镜像及手柄测试、原 input-calibration/perf-settings、shellcheck/语法检查均已通过，详情与证据见前述小节。
- 当前平板仍为 armada17 的 USB 桌面会话，Wi-Fi 可用，最近保留用户音量 15%；本轮无待运行的设备测试，无重启、分区或 Android 修改。代码提交不等于已生成完整安装镜像。
- **唯一下一步**：用显式 `ARMADA_DEVICE=oneplus-pad-2-pro` 和已校验的本地固件重建完整内核包及镜像，配套新 initramfs，用全新可写层临时启动并验证冷启动音频/服务；同时追踪早期开机 APM 超时。现有 USB 里的旧 dracut 文件仍未替换。内部安装和固化双启动继续按第 I 项另行核对。

### 19.34 armada18 完整 USB 系统镜像与保留配置更新（2026-11-04）

- **用户要求**：构建本轮修复的新镜像供其上机测试；更新 U 盘必须保留已有用户配置。用户已正常返回 Android，并把原 OPDARMADA U 盘接回电脑。e2fsck -fn 只读检查通过，卷 UUID `f7e9c75b-e475-4cee-9fc4-027bd7dfb10c`；初始只读挂载后再操作。原始设备身份和文件清单只在忽略的 `armada18/` 内保存。
- **构建输入**：源码基线 `e05c45586ed21d409a64e85297f5cdc5bc2d3f78`；固定已实测的 Plasma 6.7.5 镜像层 `3af72886188c95e62868af428b7a710d2c803ed50612e1bc618536610c06dfc8`。复用源码与 1408 逐字节一致的 audio1 编译缓存，重新打包为配套内核/1594 模块/29 DTB；没有混入新的 BTF 构建。按当前源码的内核安装、音频检查、os-release、dracut 步骤构建完整只读系统。维护镜像只覆盖系统文件；基础层已有的初始化不重复执行（直接重跑 step40 会在已删除的 login1 imply 注解上失败，失败日志保留）。额外将本机私有 GPU zap 和触控固件一并放入新系统镜像。
- **完整产物**：`root.squashfs` 7084085248 字节，SHA-256 `a64e67aafba664fa6a57d8332d1581ff82d201084b4c641d16a86f2f3f71fec1`；版本 `20261104.e05c455.usb1`；容器 ID `3fde81d95a90bb64b0fa625d88abbfebe885d2b8603aefc00631d674d117209c`。新系统内 UCM/固件检查通过，1594 个模块及 vmlinuz 与同套缓存逐个匹配，4 个手柄/设备识别源码文件与提交一致；dracut 归档已重新生成。
- **配套临时启动镜像**：`read-boot.img` SHA-256 `2077a38eccf08b59dae10a38aae9387ff9d08a259efd26758297a06e0aeef429`；EFI 内核仍为 `2fc33641…`；早期 initramfs 为 `7768f80e…`。在 armada17 的 135 个 cpio 条目中只修改 `/init`，更新 root 大小/哈希及配置保留逻辑；overlay.ko 与新系统模块一致。ESP、kShim 负载解包回读一致，ColorOS 基础内核、shim 和 U-Boot 与旧镜像逐字节相同。菜单默认仍返回 Android；本轮 kShim 项标题 `Armada 18 USB disk - source fixes - user data kept`，桌面项 `Armada 18 USB DISK - DESKTOP mode`。
- **配置保留的源码修复**：旧 USB 启动脚本每次用 `cp -af` 覆盖 bootstrap 文件，会覆盖已有配置；单纯改成 BusyBox `cp -an` 又会跳过整个已有目录，导致缺失的文件也不补。最终改为：非空既有 upper 完全不复制 bootstrap，保留用户修改、whiteout 和 opaque 删除状态；只有首次空 upper 才逐路径补缺。`tests/usbroot-preserve-config-test.sh` 覆盖配置、数据、权限、符号链接、删除状态和空/非空检测；平板实际 BusyBox 的逐项复制测试通过，独立审查通过。
- **更新方式**：`update-usb.py` 核对序列号、卷标、UUID 和旧 root 哈希；在只读挂载上备份整个 upper（含权限/xattr/ACL/whiteout，忽略已失效的运行时 socket），核对 upper 中的模块仍与同一内核匹配；再 remount rw，保留旧 root 的硬链接，只写新的 root.squashfs 与校验元数据，回读校验，写入前后及全部操作后对比 upper 清单，最后卸载。用户要求减少授权，因此这些管理员动作合并为一次 pkexec 调用。早先分开的备份授权未通过、未执行写盘；本次合并流程已成功完成：USB 回读 SHA-256 与新 root 一致，upper 的 30260 项在写入前、激活前和全部操作后完全一致。完整备份在 `usb-backup-20261104-120538/upper.tar`，SHA-256 `01c76d623975efb4befc840cf79abd47c02b46515580ca95c57e7f9b989dfaf3`；旧镜像保留在 U 盘 `opd/backups/20261104-120538/root.squashfs`。U 盘已安全卸载，最终回执 `usb-update-result.json`。
- **实机发送（用户明确授权）**：原 Android 启动完成、实际 bootconfig 为 b 槽、boot_b 哈希仍为 `0e5afd65…`，电量 88%。已执行 `adb reboot bootloader`，13.576 s 后识别 fastboot，确认 current-slot b，立即 `fastboot boot read-boot.img`；发送和 Booting 均 OKAY，总耗时约 0.628 s。已告知用户断开电脑 USB 并接入 U 盘，选择 Armada18 及桌面项。证据 `android-precheck.json`、`launch.json`、`fastboot-output.txt`。
- **验证边界**：本次完整镜像已构建并发送临时启动，后续用户已确认进入桌面，首轮结果见 §19.35。保留已有 upper 的测试不能替代全新可写层验收。旧 armada17 引导中的 root 哈希不同，必须使用 armada18 配套 read-boot.img。测试步骤与产物身份见 `armada18/README-TEST.md`、`inputs.json`、`deployment.json`、`boot-validation.json`、`SHA256SUMS`；旧失败日志仍保留。

### 19.35 armada18 首次校验等待与桌面启动验证（2026-11-04）

- **用户反馈及判断**：用户提供屏幕照片并认为启动失败。照片显示 U 盘已识别、ext4 已挂载，停留在 `checking image SHA-256...`，未出现 digest mismatch、模块失败或 panic。检查 `/init` 确认 USB 校验没有 120 s 超时（该倒计时仅在 usbroot 返回后执行）；当时 SSH 不可达符合尚未 switch_root 的阶段。用户随后确认“现在进来了”，因此本次不是启动失败，而是新镜像首次完整校验的等待。
- **免重复校验机制正常**：更新系统时清除了只对应旧镜像的记录；新镜像第一次需要完整读取 7084085248 字节。SSH 核对版本 `20261104.e05c455.usb1`、新 root SHA-256 `a64e67aa…`；`root.verified` 的 size/inode/mtime/ctime 分别为 `7084085248/2356204/1793765399/1793765451`，与实际文件完全匹配。同一镜像后续启动可跳过完整读取；用户配置修改不影响 root 文件身份，不会使此记录失效。证据 `armada18/desktop-validation.txt`。
- **冷启动后的运行状态**：已进入完整桌面，Wi-Fi SSH 正常；声卡 OnePlus-Pad-2-Pro 注册，PipeWire 提供 Speakers 和 Internal microphones，保留音量 15%；`armada-controller-type.service` 返回 0。本次仍使用已有 upper，因此不算全新可写层验收；用户已确认本轮网页视频出声、音量和静音均正常；同时反馈最大响度低于原版 Android，见 §19.36。
- **发现并修复维护镜像权限遗漏**：`armada-installer-visibility.service` 首次启动报 203/EXEC，原因是维护镜像直接复制 system_files 时没有执行原 step40 的 chmod。8 个脚本在源仓原为 0644，标准完整构建依赖 step40 设为 0755。本轮已把这 8 个源码脚本标记为可执行，维护镜像配方也补上同样权限归一化，并修复当前系统中的权限；仅修改系统脚本权限，不修改用户设置。服务重启后 `active (exited)`、failed units=0、system running，见 `installer-mode-hotfix.txt`。
- **镜像与持久层边界**：当前 U 盘 a64e67aa…只读镜像仍为首轮产物；权限修复已持久保存在其 upper，并在源码/后续构建输入中修复，尚未重出另一份 root.squashfs。重新发布独立镜像时必须包含该权限修正，不能把运行中修复冒充镜像内已修复。当前校验记录仍有效，无需因这个权限修复再次全盘校验。
- **当前设备状态与下一步**：平板在 armada18 桌面，Wi-Fi 可调试，U 盘保持连接，服务失败项为 0。下一步先由用户确认本轮网页声音/音量/静音和原配置；随后重出包含可执行权限修正的独立镜像，并另做全新可写层验收。尚未固化内部双启动。

### 19.36 armada18 音频用户验收与最大响度（2026-11-04）

- 用户确认本轮网页视频出声、音量和静音“都正常”，首轮保留配置的桌面音频验收通过；同时反馈最大音量低于 Android。
- 通过 Wi-Fi 只读取证：六颗 Amp0/1/2/3/5/6 的 `PCM Playback Volume` 全部为 270，驱动公开的 TLV 为 -90 dB 起、每步 0.25 dB，所以实际功放设定为 -22.5 dB；DSP stream0 gain=8192（Q13 unity），PipeWire 软件音量=1.00。最大响度差异的明确来源是 §19.30 为未接入厂商保护链路而设置的保守增益，而非桌面滑块没有到100%。
- 相比同 ACF/参考原厂配置中使用的350/-2.5 dB，当前降低了20 dB。该对比是配置增益差，不是本轮对 Android 最大声压的重新测量，也不能解释为主观响度的固定百分比。参考项目记录原厂有扬声器保护及每路调音，当前拓扑只有基本混音/格式转换/TDM链路；本机当前 ColorOS 的具体保护模块、模型和阈值尚未独立核验，见 §19.37。后续应单独完成保护、调音和产品增益验证，再决定是否提高上限。
- 本轮没有修改增益或音量，仅解释现有保守设置；270 不是经过扬声器热/机械测试认证的安全阈值。该项继续登记于统一音频技术债表。系统当前 failed units=0，仍在 armada18 桌面可通过 Wi-Fi 调试。

### 19.37 功放自身保护、Android 保护与参考仓库策略（2026-11-04）

- **物理风险与判断边界**：较大持续功率会使音圈过热，低频大振幅可能超过振膜/悬挂的机械行程。提高增益并不意味着必然损坏，也没有证据证明350必然损坏或270对所有内容都安全。DSP 数字增益超过 unity 后还可能先发生削波；这是数字信号失真，不能直接等同于扬声器损坏。
- **硬件能力**：[AWINIC AW88261 官方资料](https://www.awinic.com/en/productDetail/AW88261FCR)列出短路、过温、欠压、过压保护，以及扬声器电压/电流采样反馈；芯片自身无 DSP。这些芯片保护不能等同于按具体扬声器校准的温度/振幅保护。本轮没有读取运行时所有保护寄存器，不把“缺少原厂 DSP 保护”写成“硬件保护全部关闭”。加载原厂 ACF 也不等于恢复外部 DSP 算法和反馈链路。
- **Android 原理与证据边界**：Smart Amp 系统可结合电压/电流反馈、扬声器参数和模型，动态限制持续功率与位移，允许短时较大峰值；参见 [TI 原理说明](https://www.ti.com/document-viewer/lit/html/SSZT848/GUID-27E3C581-A61B-4BFE-AB84-CE29D404C047)。这是通用机制，不据此声称本机 ColorOS 已逐项验证使用了这些算法；本机具体原厂模块和标定值仍待提取核验。
- **参考项目实际策略**：只读核对 `../onepluspad-2pro-mainline` HEAD `9762c0994e2b5acd672be2ddbda680fd61f1cad6`。`packaging/erhai-audio/tree/usr/share/alsa/ucm2/Qualcomm/sm8750/OnePlus-Pad-2-Pro/Speaker.conf` 使用六路350/-2.5 dB、DSP8192/unity，注释明确缺少原厂保护及每路调音，高音路仍收到全频信号。HiFi 只在 `/run/erhai-audio/speaker-enabled` 存在时提供扬声器，每次启动需手动 `erhai-audio-speaker enable`；README.zh-CN.md 第135行报告听音位置、响度和音质实体验收通过，同时明确“无原厂扬声器保护”。提交 `c69ae8589c94bf8bb9b860b303f6ba16b00242d8` 将原先270提高到350；听音通过不等于长期热/机械边界验证。
- **本轮结果与下一步**：未改变平板增益、镜像或参考仓库。Armada 当前选择270并自动提供扬声器，参考项目选择350并默认关闭，属于不同的启用策略；不能把两者任一策略视为已实现原厂完整保护。后续产品响度调整需记录每路频响/限幅与保护验证，独立于现有音频通路验收。

### 19.38 按参考项目对齐扬声器增益（2026-11-04）

- **用户决定与范围**：用户明确“按照 ../onepluspad-2pro-mainline 的来就行”，本轮按上下文将响度配置对齐：六路 Amp0/1/2/3/5/6 均350/-2.5 dB，DSP8192/unity。保留 Armada 已验收的桌面自动提供扬声器行为，不重新引入参考项目的每次启动手动开关。
- **源码与镜像输入**：修改 `system_files/usr/share/alsa/ucm2/Qualcomm/sm8750/OnePlus-Pad-2-Pro/Speaker.conf` 及拓扑 README，参考版本为 `c69ae8589c94bf8bb9b860b303f6ba16b00242d8`；正常 `system_files` 安装流程会把它带入后续镜像。现有 a64e67aa…只读镜像不因此自动改变。
- **上机预期和判据**：独立复核后，通过 Wi-Fi 在平板现有 USB 持久层原子更新同一份 UCM，备份旧文件；确认无活动播放流后受控重启 WirePlumber，丢弃旧 EnableSequence 缓存并自动应用六路增益，不主动播放测试音、保留用户软件音量与静音状态。六路读回均350、DSP8192、UCM可解析、Speakers/Mic仍存在且 failed units=0 即运行中配置验收通过；不将其表述为新镜像冷启动或听感验收。证据目录 `output/opd2413-stage4/audio3/`。
- **独立复核与源码检查**：独立 reviewer 核对六路350/DSP8192与参考一致，并指出 WirePlumber 缓存及 HiFi 旧注释；两项均已处理。`tests/oneplus-audio-image-test.sh`、`tests/alsa-ucm-duplicates-test.sh` 和 `git diff --check` 通过；内核、拓扑二进制和固件未变化。
- **运行中结果**：通过 Wi-Fi 完成原子部署并重启 WirePlumber，UCM解析成功、六路自动读回350、DSP8192、Speakers/Internal microphones仍存在、failed units=0。软件音量仍为左右65536（100%），静音仍为false；未主动播放声音。旧文件和音量状态备份在设备 `/var/lib/armada/porting-backups/audio3-20261104T050701Z/`，证据为 `audio3/deploy-remote.py` 和 `audio3/deploy.log`。
- **一致性与持久性**：设备与源码的 Speaker.conf SHA-256=`ba1185f13e9cf76e51302e5f3db854bf4332d2de69da180d157fe88d9dabbbeb`；HiFi.conf SHA-256=`46fee9fbd7f63bc9ac6e34f13b46058cbc791132e6b1f5604cd75c79df463355`。设备改动在 USB upper 中持久保存，正常源码构建及 armada18 本地维护构建输入也已更新。现有 a64e67aa…只读镜像仍是旧增益，须重建后才可独立携带本轮配置，不把 upper 部署算作镜像已更新。
- **用户验收与复核**：用户随后确认新音量“可以了”。再次只读检查六路仍为350、系统 `running`、failed units=0。新音量听感验收通过；证据仍以原部署日志及用户反馈为准，不把本轮扩大为长期满音量/热/机械测试。
- **状态与下一步**：源码、运行中配置和用户试听验收通过，新镜像冷启动仍待验证。设备在 armada18 桌面、Wi-Fi可调试、无失败服务，当前无阻塞。下一步重建包含权限及350增益的独立镜像，再做全新可写层验收；原厂保护/调音仍在统一技术债表中。完整交接见第21节。

### 19.39 armada19：独立镜像、全新可写层与 kShimC 写入 boot_b（2026-11-04）

- **镜像**：源码 `fb483bf`，在固定 Plasma 6.7.5 基础层 `3af72886…` 上按 20/41/52/55 步骤构建，内核包与 armada18 同为 `2fc21ced…`（e05c455 之后内核/`build_files` 未变）；构建只检查脚本权限而不再 chmod，47 个脚本均为 0755。镜像 `8c851151…`，`root.squashfs` 7084253184 字节、SHA-256 `1768f47742a3529e7de9ade61a83e2a9a6ca470a47c2f11ce6fd87f0e0ebc24c`，版本 `20261104.fb483bf.usb2`。镜像内 Speaker.conf `ba1185f1…`、HiFi.conf、拓扑与私有触控/GPU zap 固件哈希均核对。产物 `output/opd2413-stage4/armada19/`（`README-TEST.md`、`SHA256SUMS`、`deployment.json`）。
- **启动脚本改动（源码）**：`smoke_initramfs.py` 新增 `opd.layer=NAME`，可写层改为 `opd/layers/NAME/{upper,work}`，名字仅允许 `[A-Za-z0-9_-]`，默认 `opd/upper` 不变；读取 cmdline 改用 shell 内建 `read`。`netroot_overlay.py` 的测试默认文件加入 `90-opd-no-suspend.conf`（与旧 upper 中文件同为 `77d7be49…`），否则全新可写层会允许已知会死机的挂起。这是测试临时措施，产品级挂起方案（Armada fake suspend 或修复 s2idle）仍待做。`tests/usbroot-preserve-config-test.sh` 覆盖层选择/拒绝非法名，并可用 `USBROOT_TEST_SH` 以平板 BusyBox 运行，两种 shell 均通过；shellcheck 通过。
- **boot**：`read-boot.img` `fbfe51c0…`（94.8 MiB）相对 armada18 只改 initramfs 的 `/init` 与新增的禁止挂起文件，内核/DTB/U-Boot/kShimC/ColorOS 基础内核逐字节相同；systemd-boot 新增 `DESKTOP - FRESH test layer`（`opd.layer=fresh19`），原桌面项改名 `DESKTOP mode - your settings`。
- **U 盘更新**：`run-update.sh` 一次 pkexec 完成 e2fsck -fn（前后均干净）、只读挂载、upper 完整备份（`usb-backup-20261104-133730/upper.tar`，`3099fef1…`）、暂存并回读新 root、73019 项 upper 三次比对不变，旧 root `a64e67aa…` 硬链接保留在 U 盘 `opd/backups/20261104-133730/`。
- **全新可写层**：首启完整校验后进入桌面（屏幕停在 `checking image SHA-256...` 数分钟属正常）；`running`、失败单元 0，controller-type/installer-visibility 返回 0；六路自动为 350，Speakers/Internal microphones 自动出现，用户确认网页声音、音量、静音正常；fresh19 upper 中没有 `usr/libexec`、`usr/share`，即权限与 UCM 修复来自镜像本身（`fresh-validation.txt`）。再次启动跳过完整校验（内核 55 s 已进桌面），Wi-Fi 与音量持久（`fresh-reboot-validation.txt`）。
- **原配置**：`your settings` 项进入用户原桌面，Steam 数据（6.0 GB）、Wi-Fi、音量保留，`running`、失败单元 0。upper 中 29 个覆盖文件里与镜像重叠的脚本/UCM/拓扑/固件全部与新镜像逐字节相同，其余为测试单元、与源码相同的设备配置/ACF 和未被引用的旧 `DisplayPort.conf`（`settings-validation.txt`、`upper-overrides.txt`）。
- **USB 3**：平板端插头翻转 180° 后 U 盘为 5000 Mbps，原方向 480 Mbps，再次确认 §19.12 的方向问题。
- **kShimC 写入 boot_b（用户批准）**：第一次直接 `fastboot flash boot_b read-boot.img` 后停在 fastboot START，b 槽 retry 6→5（ABL 尝试过启动）。原因：`boot` 是 AVB 链式分区，`boot_b` 末尾保留原厂 footer，指向偏移 38338560 的 boot vbmeta（2368 字节）；94.8 MiB 镜像覆盖了它，ABL 读到无效 vbmeta 拒绝启动。`fastboot boot` 不经过分区 footer，故临时启动一直正常。已用备份刷回 `0e5afd65…` 恢复，Android 正常（`attempt1/`）。修正：`read-boot-flash.img` 为整分区镜像（`9d91e3a9…`），前部为原 boot 镜像，原 vbmeta 原样放在 99401728，footer 改写为新偏移/大小。刷入后出现 kShimC 菜单，ColorOS 正常，整分区回读哈希一致，用户确认 Android 功能正常；之后从 Android 重启选择 Armada 无需电脑（`boot-b-flash.json`）。a 槽 `slot-unbootable=yes`，ABL 不会回落到 a 槽。**任何写入 `boot_b` 的镜像都必须做同样的 vbmeta 迁移**；即使最终 boot 只含 ColorOS 内核+kShimC+U-Boot（约 41 MiB）也超过 38.3 MB。
- **boot 大小**：ColorOS 内核 36.6 MiB、kShimC 4 MiB、U-Boot 0.5 MiB、内嵌 ESP 53 MiB（Armada 内核 20.9 MiB、initramfs 23 MiB，其中 41 MB 未压缩为早期 USB 所需 ADSP/SOCCP 固件，另有 8.4 MiB FAT 空闲）。下次重出镜像时缩小 ESP；彻底缩小依赖内部 ESP（§21.2）。
- **其它观察**：`qcom-iris` 视频固件加载失败（armada17 起已有）、开机一次 APM CMD 超时、一次 GMU fenced write 延迟、powerdevil ddcutil 无 i2c 权限，均不影响本轮验收。
- **状态**：设备在 armada19（原配置）桌面，Wi-Fi 可调试，U 盘留在平板上。每次换 Armada 启动文件（内核/initramfs/root 哈希）都需要重写 `boot_b`，可在 Android 中用 root 写入并回读，无需电脑。

### 19.40 Linux 内部 UFS 只读探测通过（2026-11-04）

- **方法**：`smoke_initramfs.py --ufs-probe` 加入 `ufsprobe`：仅在 `opd.ufsprobe` 下运行，UFS 盘一出现即对整盘和每个分区 `blockdev --setro`（内核 `bdev_read_only()` 对分区继承整盘标志，写 bio 在 blk-core 被拒），之后记录主机/设备描述符、每个 LUN 的容量/扇区、GPT 主表前 24 KiB 与备份表末 20 KiB 的 SHA-256、所有 ≤128 MiB 分区的 SHA-256、前后 diskstats 和内核日志，写到 U 盘 `opd/probes/ufs-N.txt`，最后删除 UFS 的 SCSI 设备再 switch_root，桌面系统看不到内部存储。`tests/ufsprobe-report-test.sh` 用伪造 sysfs/dev 验证只读先于读取、只处理 UFS 盘、哈希与删除正确，在平板 BusyBox（`52151e7f…`）上通过。
- **boot**：`output/opd2413-stage4/ufs1/`，root 仍为 armada19；只改 `/init`，ESP 新增 `board4.dtb`（仅 UFS host/PHY 两个 `status` 改为 okay）和 `UFS READ-ONLY PROBE` 项，其余项仍禁用 UFS。`boot_b` 经无线 adb 在 Android 中以 root 写入（`android-write-boot-b.py`：推送→设备端校验→dd→清缓存回读整分区），`9d91e3a9…` → `368a4efc…`，vbmeta 已迁移。
- **结果（`ufs1/ufs-1.txt`）**：三星 KLUFG4NHHB（UFS 4.0，rev 1200），6 个 LUN 5 s 内出现，MCQ 9 队列，ICE v4.0.3；6 盘、129 个分区全部 `ro=1`，**写入次数/扇区/flush 全为 0**；6 个 LUN 容量、4 KiB 逻辑扇区与备份一致，**12 份 GPT 主/备份表与 11-02 备份逐字节一致**；119 个分区哈希中两个槽位的 boot/init_boot/vendor_boot/dtbo/recovery/vbmeta 全部一致（boot_b 为新写入值）；仅 persist、oplusreserve1/3/5、spunvm 不同，回到 Android 再读时 persist/oplusreserve1/5/spunvm 又变化，确认为 Android 运行时写入。无 UFS 错误、无 SMMU fault；寿命估计 0x01。链路当前为 HS-G2（UFS 4.0 应可到 G5，性能问题以后看）。删除设备后 UFS 主机 runtime suspend，系统 running、失败单元 0。重启回 Android 正常，Android 内核日志无 ufshcd 错误（`android-after-probe.txt`）。
- **存储布局事实（只读）**：6 个 LUN 均无未分配空间；userdata 在 LUN0 末尾（460 GB，已用 153 GB），f2fs + FBE + 元数据加密（密钥在 TEE），Linux 无法读取、f2fs 不能在线缩小；Virtual A/B（压缩）。机内安装 Armada 必须从 userdata 取得空间，见 §21.2 第 3 项。

### 19.5 最新产物和复查入口（第四轮；第五轮见 19.6）

| 内容 | 位置 |
|---|---|
| 已通过的最新临时 boot 镜像、菜单配置 | `armada4/read-boot.img`、`armada4/read.cfg` |
| 启动耗时、返回后的槽位/boot 哈希、实机结论 | `armada4/launch.json`、`armada4/android-return.json`、`armada4/hardware-validation.json` |
| 无复位位损坏的实时日志 | `armada4/usb-console.log` |
| 原始 RAM 日志、可校验状态 | `armada4/pmsg-ramoops-0`、`armada4/console-ramoops-0`、`armada4/verified/{initial,settled,final}.txt` |
| 最新内核与配置 | `armada-port/source/arch/arm64/boot/vmlinuz.efi`、`armada-port/source/.config` |
| 最新内核构建与验证 | `armada-port/usb-kernel-build.log`、`armada-port/usb-kernel-validation.json` |
| 已核对的 60 份固件 | `armada-port/firmware/manifest.json` 中 59 份 ADSP/SOCCP；`armada-port/touch-firmware.json` 中 1 份触控固件 |
| 最新 RAM initramfs | `armada-port/usb-log-initramfs.cpio.zst`（23931887 字节） |
| 第一版显示/触控内核对照 | `armada-port/display-touch-vmlinuz.efi`；已打包的 `armada1/`、`armada2/` 仍保留 |

上表缩写路径均相对 `output/opd2413-stage4/`，该目录被 Git 忽略。当前只完成内核 EFI 二进制及部分模块构建，**完整 Armada 内核模块包与完整 rootfs 尚未构建**；主机只读检查未发现已有 Armada Podman 镜像。不要把 BusyBox RAM 用户态当作完整 Armada。

重新核验保存的状态，无需重启平板：

```sh
python3 porting/oneplus-pad-2-pro/linux/decode_smoke.py \
  output/opd2413-stage4/armada4/pmsg-ramoops-0 \
  --out output/opd2413-stage4/armada4/verified
```

后续 USB 日志接收使用 `linux/capture_usb.py --out <日志文件>`，在重启前开始监听。接收器只匹配 `1d6b:0104`、序列字符串 `armada-opd-smoke`；若输出 `permission_needed`，按它报告的实际节点和 UID 用 `pkexec setfacl` 添加临时访问权限。本轮实际为 `pkexec setfacl -m u:1000:rw /dev/ttyACM0`；不能假定下一轮仍为这个节点。设备断开后该临时节点消失，不修改主机的永久 udev 规则。

## 20. 后续测试顺序与通过标准

**本节20.1–20.3保留早期测试设计作历史对照；最新状态及唯一下一步以第21节为准。USB载体、完整用户空间、触控、GPU、Wi-Fi等已完成的项目不因下表旧文字而重新列为未实现。20.4的镜像验收原则继续适用。**

### 20.1 先确认试启动载体

下一次继续时，先确认是否有可用于测试的 USB 存储及 OTG/Hub；尚未获得这个信息。可选 USB 存储，或先做 USB 网络/RAM 用户空间。最终是否安装到内部独立空间也未决定。确认“有设备”不等于已选定格式化/安装目标。

现阶段继续使用已验证的 **RAM ESP Blob + EFI 内核 + 早期固件 initramfs**。Linux USB 设备模式已通过，**U-Boot 的 SM8750 USB 主机驱动和从 USB ESP 直接读取 EFI 文件尚未实现/验证**；不能假设 U-Boot 已能读取外接 U 盘。

### 20.2 优先测试清单

| 编号/顺序 | 准备与动作 | 通过标准及保存证据 |
|---|---|---|
| A：完整内核包与 rootfs，离线先做 | 基于已验证 7.2.6 源码构建完整模块、DTB 和 Armada 内核包；准备匹配的 aarch64 Armada rootfs。保留当前已通过镜像作对照，编译保留至少一核 | 内核/模块版本与配置一致，依赖闭合；DTB 与固件清单可复核；记录镜像来源、版本和 SHA-256，不混用别的构建的模块 |
| B1：USB 主机/存储，仅选择 USB 存储路径时 | 用户指定测试设备并接 OTG/Hub；Linux 仍禁用内部 UFS。先枚举外设与容量，再将测试块设备设为只读，读取少量固定数据；不格式化、不挂载为可写 | USB role 为 host，USB/SCSI 设备稳定枚举；读取数据哈希与主机对照一致；无 PHY/IOMMU/USB 错误，复位后 Android 回归正常。ACM 通道随 USB 角色改变而断开时，用带校验的 pmsg 留证 |
| B2：USB NCM 网络/RAM（**已选定，第五轮已通过**） | 为 RAM initramfs 增加 NCM 与临时网络配置，先测小文件再测分块大文件；每块和整体做长度/SHA-256 校验 | 双向网络及持续传输正常，无停滞/损坏；镜像完整下载到 RAM。ACM 成功不能代替网络测试；不写 Android 分区 |
| C：完整 Armada 用户空间（**第六轮基本通过**，触控固件待补） | 在 B1 或 B2 通过后，从选定载体启动匹配 rootfs；首次采用只读根与 RAM 可写层，继续禁用内部 UFS，保留超时返回路径 | `/etc/os-release` 为 Armada，systemd 启动到预定 target，可到登录界面/桌面；保存系统、内核、存储和服务日志；OEM ABL 更新保护仍生效 |
| D：桌面显示与触控映射 | 在真实图形会话检查方向、分辨率、背光、屏幕中心/四角点按、滑动、多点；有手写笔再单独测笔 | 图形会话稳定，坐标与屏幕位置一致，没有旋转/轴向错误。当前只证明 evdev 事件可读，尚未验证 libinput/桌面映射 |
| E：GPU 加速 | 补齐同机签名 GPU 固件及 SQE 固件，核对已有 Armada GPU 驱动与 erhai 差异，运行实际 EGL/OpenGL/Vulkan 检查 | 明确识别实际硬件渲染器并完成渲染测试，无 GPU/IOMMU fault 或 hang；`renderD128` 存在本身不算加速成功。可先用软件渲染完成 C/D |
| F：PCIe 与 Wi-Fi | 先排查当前 `1c00000.pcie: cannot initialize host` 的模块、供电和 PHY 供应者，再补齐 WCN7860 固件/板级数据；由用户在系统界面连接网络 | PCIe 链路稳定，Wi-Fi 可扫描、连接并持续传输，重连正常；不以参考项目已验证代替本机证据 |
| G：其余外设 | pogo 键盘、亮度/背光、电量/充电、音频、蓝牙、手写笔、摄像头逐项测试；有模块未移植则先补驱动/固件 | 每项分别保存日志与实际操作结果。ADSP 运行不等于音频/充电已通过；pogo 模块编译通过不等于实体键盘通过 |
| H：稳定性与 Android 回归 | 完整系统基础可用后，再做休眠/唤醒、空闲、拔插 USB、冷启动和多轮 Android/Linux 切换 | 无复位/存储/显示/GPU 故障；每轮核对 b 槽、原 boot 哈希及 Android 解锁、指纹、Wi-Fi、KernelSU/susfs，记录异常而非仅记录能开机 |
| I：安装和固化双启动，最后做 | 先形成可审阅的介质/分区布局、备份、最终镜像、更新与回滚方案，再确认具体写入目标；沿用保留 OEM ABL 的链路 | 最终镜像默认 ColorOS、菜单选择 Linux 可用；掉电/异常回退及升级不覆盖 Android 关键分区；多轮正常重启通过后，才把阶段五标为完成 |

顺序建议为 **A →（B1 或 B2）→ C/D → E/F/G → H → I**；GPU/Wi-Fi 排查可与 rootfs 离线准备交替，但不能跳过载体和完整用户空间测试，直接进入内部安装。

### 20.3 每轮通用记录和边界

- 记录代码提交、内核/DT/initramfs/boot 镜像 SHA-256、配置差异、实际选中的菜单项、启动耗时与退出路径；新镜像继续做负载解包比对和适用的离线/QEMU 检查。
- 保留 ColorOS 为默认项；主机把 `adb reboot bootloader → fastboot getvar current-slot → fastboot boot` 放在同一连续脚本中，避免人为拉长原厂 START/Recovery/Power off 菜单停留。最近几轮识别 fastboot 约 13.5 秒，识别后约 0.7 秒内发出 boot。
- RAM 内核探测默认约 120 秒自动返回；完整 rootfs 测试如需更长窗口，先明确本轮超时/退出方式，并提前约好需要用户选菜单的时间。不要因日志暂时不可读就强制电源复位，导致证据丢失。
- 继续同时保留 USB 实时日志与 25 份 pmsg 状态；只接受完整 SHA-256 验证的恢复结果。保存的原始 RAM 文本可能有位损坏，不能直接据此宣布通过。
- Linux 内部 UFS 仍保持关闭，直至另行准备专门的只读存储测试。不得把 U-Boot 的只读 UFS 成果当作 Linux UFS 驱动已通过；也不运行 erhai 的分区/刷写/切槽安装脚本。
- 始终保留槽位 b 与已备份的原 boot 对照，不切换旧槽 a，不替换 OEM ABL。涉及最终安装前，先完成具体目标、恢复文件和写入内容的核对；当前尚无已选定的可写目标。
- 只提交实测/离线检查通过的对应代码。按本轮用户要求更新并提交方案文档和源码；原始调试记录与私有固件继续留在本地，不纳入提交。

### 20.4 最终安装镜像的持久化要求（用户明确，2026-11-04）

用户后期要用镜像安装系统；所有验收通过的修复必须进入镜像的可复现构建输入，不能依赖现有测试 U 盘的 overlay、手工 SSH 修改或 `/run` 临时文件。“测试 U 盘重启保留”不能作为“安装镜像已包含”的判据。

音频当前构建覆盖情况：

| 内容 | 当前是否进入镜像构建输入 | 最终要求 |
|---|---|---|
| §19.30 UCM 修复 | 是，已放在 `system_files` 工作树，`Containerfile` 的 ctx COPY 与 `40-vendor-system-files.sh` 会安装；纳入本轮源码提交 | 随源码保存，镜像内校验文件及配置匹配 |
| §19.29 audio1 内核修改 | 是，§19.31 的 1408 补丁已加入 series，逐字节还原实测源码 | 使用新内容哈希的内核包，内核/模块同套构建 |
| OnePlus AudioReach 拓扑 | 是，§19.31 保存 m4、宏、固定上游、生成器及实测 blob，已验证可重建 | 镜像构建检查拓扑 SHA-256 |
| AW88261 ACF 和设备专用 ADSP 固件 | §19.31 提供准备工具和哈希清单，本地 60 份输入已准备；二进制继续 Git 忽略 | 显式 OnePlus 构建目标要求全部输入匹配，否则失败；不公开私有固件 |
| initramfs | §19.31 构建脚本已显式收录 OnePlus 固件/拓扑并检查归档；当前 U 盘旧 dracut 文件未替换 | 完整新镜像用同套内核/模块重建，冷启动验收 |

**最终验收门槛**：从保存的源码和明确的私有输入重新构建镜像，检查内核、模块、DTB、拓扑、UCM、固件和 initramfs 一致；使用没有现有 USB overlay/ALSA 保存状态的全新可写层临时启动，验证冷启动后 Speakers/Mic 自动出现、网页播放、音量和静音，再允许标记“镜像安装后音频可用”。内部存储安装仍须按 §20.2 I 项单独核对目标和恢复方案。源码及构建检查已按 §19.31 落地，完整安装镜像尚未重建和实机验收。

## 21. 最新验收与脱离 U 盘的机内双启动缺口（2026-11-04）

### 21.1 本轮提交范围与当前实机状态

- 用户确认350增益可用，要求提交并说明后续工作。本轮包含：USB启动保留已有upper配置/数据/删除状态及回归测试；8个脚本的可执行权限；六路350/-2.5 dB、DSP8192及对应文档。私有固件、U盘备份、镜像和原始日志不纳入提交。
- 已验证：USB完整桌面/游戏模式、显示/触控/磁吸键盘及触控板、GPU基础加速、Wi-Fi、网页音频/音量/静音、新增益听感；无内置手柄的服务误报已修复。显示接管此前16/16临时启动通过。它们分别有对应场景证据，不能自动扩展为休眠或长期稳定性通过。
- 当前实机仍为 `20261104.e05c455.usb1`，root为overlay，底层是U盘上的squashfs；最新权限/音频修正在USB upper中。`bootc status --json`显示 `status.booted=null`、`staged=null`、`rollback=null`，不是已部署的正式OSTree系统；`/proc/partitions`只有USB sda/sda1、loop和zram，没有内部UFS块设备。系统 `running`、失败服务0，Wi-Fi可调试。
- 最新源码检查：USB配置保留测试通过；initramfs生成器Python语法通过；8个脚本均为0755且各自Bash/Python语法通过；本轮增益修改后的OnePlus镜像输入检查、UCM重复节检查和diff空白检查通过。已有USB升级流程保存30260个upper条目不变，证据见§19.34；最新独立镜像的全新upper验收仍未完成。

### 21.2 必须完成的机内启动路径

最终目标为：开机 → 当前槽位boot中的kShimC菜单 → 默认Android；选择Armada时经U-Boot/EFI读取机内启动文件，再由Linux挂载机内根与用户数据。拔掉U盘、断开电脑后两个系统都可启动。以下为尚未完成的关键工作；第4节阶段五和第5节中的早期分区/OTA设想不能直接当作本机已验证安装步骤。

| 顺序 | 当前基础与缺口 | 完成标准 |
|---|---|---|
| 1. 最新独立镜像（§19.39 已通过） | armada19 在固定基础层上从源码输入构建，全新可写层首启/再启和旧配置启动均通过；尚未从零（含基础层）完整重建 | 后续基础层更新时按同一判据从零重建验收 |
| 2. Linux内部UFS（只读部分 §19.40 已通过） | Linux 只读探测：6 LUN/4KiB/GPT/分区哈希与备份一致，写入 0，无 I/O/SMMU 错误，回 Android 正常；写入/持久性测试需要专用 Linux 空间 | USB根保持不变，先用单独候选启用Linux UFS，只读核对6个LUN、4KiB逻辑扇区、容量/GPT及选定数据哈希，核查I/O/SMMU错误，回Android验证；之后才对专用Linux空间做写入/持久性测试 |
| 3. 内部空间与安装方案 | 尚无Armada机内ESP/root/data空间；userdata占据所在LUN尾部。通用 `armada-installer` 首装会重建/缩小userdata并清除Android用户数据，不能直接沿用 | 给出本机具体布局、容量、备份/数据迁移和恢复方案，逐分区列明写入范围。保留Android系统与保留Android现有数据分别验收；目前没有通过验证的无损安装方案 |
| 4. 正式根部署与机内启动文件 | 当前USB squashfs+overlay不满足现有安装器要求的booted OSTree源；早期initramfs还按USB标签找根，ESP/内核/initramfs目前打包进临时boot | 形成可安装且可更新回滚的内部部署；initramfs按稳定UUID/PARTUUID挂载内部根与持久数据；U-Boot从机内ESP加载匹配内核/initramfs/DTB，不再等待USB。先保留fastboot临时启动进行验证 |
| 5. 固化kShimC | 已写入 `boot_b`（§19.39，保留 AVB vbmeta），菜单/ColorOS/Armada 均可从闪存启动；当前 boot 仍内嵌 ESP | 使用与当前Android匹配的最终boot，先临时启动核对两项；最后按明确写入范围固化当前槽位，不替换OEM ABL。拔U盘和电脑，冷开机至少10轮覆盖两系统选择、默认Android及失败恢复 |
| 6. 更新、回滚与恢复 | 已有EFI同步及bootc相关脚本，但未在本机正式内部部署上走通；Android OTA后kShimC及槽位维护未验证 | Armada内核/initramfs/DTB同步更新，失败可选旧部署；ColorOS升级/槽位变化后可恢复菜单且保留Android；原boot和分区表备份可核验，救援路径有实际演练 |

当前临时boot约94.8MiB，已接近96MiB分区上限；最终方案优先从机内ESP加载可更新的Linux内核/initramfs/DTB，避免持续向boot内塞完整Linux启动文件。具体落地仍以本机测试为准。

### 21.3 仍需完成的功能和稳定性测试

| 范围 | 待测试或待完善内容 |
|---|---|
| 新镜像与持久化 | 全新upper冷启动、再次启动跳过有效校验、镜像替换后重新校验；旧配置升级保留及回滚。独立新镜像必须自行包含修复，不靠现有upper |
| 电源与日用稳定性 | 充电/电量/温控、关屏与亮度、电源键、挂起/唤醒、待机耗电、低电量和持续负载；启动参数仍有clk/pd/regulator ignore-unused及selinux=0等调试设置，发布前需逐项处理或明确保留原因 |
| 音频 | 六路扬声器位置/左右、麦克风实际录音及增益、播放与挂起恢复；原厂保护及每路调音仍为§19.30统一技术债，当前按参考配置的可听验收不等于这些已完成 |
| 网络、输入与游戏 | Wi-Fi断线重连/休眠重连、蓝牙与蓝牙音频、GPU/实际游戏长时负载、外接手柄和桌面/游戏模式切换；手写笔、摄像头、自动旋转按需求分别验收 |
| USB与已知清理项 | USB 3方向切换、USB/Hub/充电组合；开机APM超时及1406无效teardown清理。USB 3不是最终机内根的前提，但其余接口使用仍需测试 |
| Android回归与维护 | 两系统多轮冷启动/切换后，复查Android解锁、指纹、Wi-Fi、充电及既有KernelSU/susfs；核对预期boot内容和非目标分区未被更改，再验证更新与恢复 |

**唯一下一步**：确定机内空间方案（§21.2 第 3 项）；Linux UFS 只读探测已通过（§19.40），仍保持 USB 根不变、不写内部介质。独立镜像与全新可写层已验收、kShimC 已写入 `boot_b`（§19.39）；设备在 armada19 桌面，U 盘留在平板上，换启动文件时需按 §19.39 重写 `boot_b`。
