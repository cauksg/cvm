# xs-cvm

`xs-cvm` 是一个 RISC-V 机密虚拟机安全 I/O 通路原型，基于 QEMU、OpenSBI、Linux/KVM 和 kvmtool，验证设备绑定、运行期间的 MMIO/DMA、安全中断投递以及设备解绑。

当前已验证：**单个物理 hart 上运行两个 CVM，每个 CVM 配置两个 vCPU、直通两张 e1000e，每张网卡对应独立 TDI。** vCPU 在同一 hart 上分时运行。本文使用 CVM/TVM 表示机密虚拟机，TDI 表示分配给 TVM 的设备接口。

本原型假定 **I/O 设备和通信链路可信**，使用普通 QEMU e1000e 模拟网卡；尚未实现真实设备的 SPDM 认证、TDISP 和 PCIe IDE，不代表完整符合 CoVE-IO 标准。

## 架构与通路

```text
QEMU RV64 / 单 hart / bitmap 隔离模型 / 可信 IOMMU 与 IMSIC 资源模型
└── OpenSBI Monitor（M-mode，可信）
    ├── TVM/vCPU 私有状态、内存归属与 bitmap 切换
    ├── G-stage BAR 映射、TDI 生命周期、可信 IOMMU 配置
    ├── IMSIC file 归属与设备中断路由
    └── Host Linux / KVM / VFIO + kvmtool（不可信管理侧）
        ├── TVM A：vCPU0 + vCPU1 / e1000e × 2 / TDI × 2
        └── TVM B：vCPU0 + vCPU1 / e1000e × 2 / TDI × 2
```

三条运行通路分别是：

- **CPU 访问设备**：Guest 的设备 BAR GPA 经 Monitor 管理的 G-stage 映射到物理 BAR；MMIO 保护按 4 KiB 页管理，检查页独占及别名冲突。
- **设备访问内存**：设备 DMA 经可信 IOMMU，以 Guest GPA 作为 IOVA 映射到所属 TVM 的私有物理页；DMA 映射和设备身份由 Monitor 校验。
- **设备投递中断**：设备 MSI 经 IOMMU 的 BASIC 模式重映射到目标 vCPU 独占的 IMSIC interrupt file，使用该 file 内的 IID 标识中断；此通路不使用 MRIF。

Host 负责发现设备、准备布局和发起请求；Monitor 校验设备身份、归属、地址范围和状态转换，Guest 通过 COVG 显式接受设备。可信模式下，QEMU 的固定平台模型与 Monitor 共同限制 Host 对 IOMMU 配置、PCI 配置和受保护 IMSIC 资源的访问。

解绑时撤销设备 DMA/MSI 和 Guest BAR 映射，完成失效与资源回收。设备 IID 保留给设备通路，Host 通过 COVI 注入时不能冒充该设备中断。

## 源码导航

| 组件 | 主要实现 |
| --- | --- |
| QEMU | [qemu](qemu)：bitmap、可信平台资源、IOMMU 和 IMSIC 模型 |
| Monitor 生命周期 | [sbi_cvm.c](opensbi/lib/sbi/sbi_cvm.c)：TVM、设备绑定与 TDI 状态机 |
| 私有 CPU 状态 | [sbi_cvm_state.c](opensbi/lib/sbi/sbi_cvm_state.c)：逐 vCPU 状态保存、恢复及 Host 交换边界 |
| 内存与 MMIO 隔离 | [sbi_cvm_bitmap.c](opensbi/lib/sbi/sbi_cvm_bitmap.c)、[sbi_cvm_mmio.c](opensbi/lib/sbi/sbi_cvm_mmio.c) |
| 可信 DMA | [sbi_cvm_iommu.c](opensbi/lib/sbi/sbi_cvm_iommu.c)：Monitor 管理的 IOMMU 配置和撤销 |
| COVI 与 IMSIC | [sbi_cvm_covi.c.inc](opensbi/lib/sbi/sbi_cvm_covi.c.inc)：逐 vCPU 中断资源与注入策略 |
| Host 内核 | [vcpu.c](linux/arch/riscv/kvm/vcpu.c)、[iommu.c](linux/drivers/iommu/riscv/iommu.c)：KVM/VFIO 与 IOMMU 前端 |
| 用户态管理 | [cove-io.c](kvmtool/cove-io.c)：设备准备、绑定、启动和 teardown |
| Guest 接受设备 | [cove_io_guest.c](linux/arch/riscv/kernel/cove_io_guest.c)：COVG 枚举、报告校验和接受 |
| Guest 网络通路 | [cove_io_nic.c](linux/arch/riscv/kernel/cove_io_nic.c)：私有 DMA 与网卡验证支持 |

## 获取源码

主仓库使用 `devel` 分支，修改后的 QEMU 位于同一 GitHub 仓库的 `qemu-secure-io` 分支，通过子模块固定到对应提交：

```bash
git clone --branch devel git@github.com:cauksg/cvm.git xs-cvm
cd xs-cvm
git submodule update --init --recursive
```

以上地址需要配置 GitHub SSH 访问。其余子模块地址见 [.gitmodules](.gitmodules)，其中包含 Gitee 地址。已有工作副本更新 QEMU 子模块地址时执行：

```bash
git submodule sync -- qemu
git submodule update --init --recursive
```

请使用主仓库固定的 QEMU 提交；直接替换为上游 QEMU 会缺少本原型的 bitmap 和可信 I/O 平台扩展。

## 环境与构建

开发机需要 Linux，以及以下工具和开发依赖：

- RISC-V Linux 交叉编译器和 binutils，默认前缀为 `riscv64-linux-gnu-`。
- Git、Bash、GNU make、GCC/G++、Python 3、Ninja，以及 QEMU 所需的 GLib、Pixman、pkg-config、Python 虚拟环境支持等。
- Linux 内核构建依赖，包括 flex、bison、bc、OpenSSL 和 ELF 开发库等。
- `fakeroot`、GNU `cpio`、`realpath` 和 `sha256sum`。
- BusyBox 1.33.1 源码：从 [BusyBox 官网](https://busybox.net/downloads/) 获取 `busybox-1.33.1.tar.bz2`，放在项目根目录。

构建脚本不会安装宿主机依赖或编译交叉工具链。BusyBox 源码压缩包未纳入仓库。

```bash
tar -xf busybox-1.33.1.tar.bz2
bash build-tool.sh
bash build-host.sh
```

`build-tool.sh` 构建 QEMU、交叉编译 libfdt 并构建静态 BusyBox；`build-host.sh` 构建 Guest/Host Linux、kvmtool、OpenSBI，再打包 rootfs。已有内核配置在应用必要选项后保留；首次构建使用默认配置。

默认产物如下：

| 路径 | 内容 |
| --- | --- |
| `qemu/build/qemu-system-riscv64` | 包含原型扩展的 QEMU |
| `build-riscv64/arch/riscv/boot/Image` | Host 内核 |
| `build-riscv64-guest/arch/riscv/boot/Image` | Guest 内核 |
| `kvmtool/lkvm-static` | Host 中运行的 kvmtool |
| `opensbi/build/platform/generic/firmware/fw_jump.bin` | Monitor 固件 |
| `rootfs_guest_riscv64.cpio` | Guest initrd |
| `rootfs_kvm_riscv64.img` | Host initrd，内含 Guest 内核、initrd 和测试脚本；格式为 cpio |
| `rootfs_kvm_riscv64.img.manifest` | 打包时的版本、配置与产物 SHA-256 清单 |

可通过环境变量覆盖编译器、并行度和内核输出目录，例如：

```bash
CROSS_COMPILE=riscv64-linux-gnu- JOBS=16 \
HOST_BUILD_DIR=build-host GUEST_BUILD_DIR=build-guest \
    bash build-host.sh
```

Host 与 Guest 必须使用不同输出目录；启动时也需指定相应的 `HOST_BUILD_DIR`。下文的统一回归入口使用默认产物目录，建议验证时保留默认目录。

只更新 Guest 时，可运行 `bash build-guest.sh`；这不会更新已打包到 Host 镜像中的 Guest。之后运行 `bash scripts/cp.sh` 重新打包。修改 kvmtool 等编译产物时，应先重新编译，再打包。打包脚本不编译源码。

不要并发构建或打包同一组输出文件。manifest 是打包快照，不证明旧二进制来自当前源码；源码修改后应重新构建对应组件。

## 运行安全 I/O 通路

建议从完整的双 TVM、四网卡测试开始：

```bash
HOST_COVE_IO_TRUSTED=1 \
HOST_APPEND_EXTRA='kvm.cvm_state_attack_test=1' \
HOST_COVE_IO_TEST_WAIT_SECS=540 QEMU_TIMEOUT=660 \
    bash scripts/run-cove-io-test.sh multicvm-smp-nic-pair
```

`HOST_COVE_IO_TRUSTED=1` 启用固定平台可信 IOMMU 和 bitmap 模型。启动脚本自动配置四张 e1000e 和测试网络；Host autorun 完成 VFIO 准备、两个 TVM 的创建和验证。无需手动添加测试网卡。

此模式的拓扑为：

| TVM | 内存 | vCPU | Host PCI 设备 | 目标 IMSIC file |
| --- | --- | --- | --- | --- |
| A | 256 MiB | 0、1 | `00:02.0`、`00:03.0` | 分别使用 file 1、2 |
| B | 256 MiB | 0、1 | `00:04.0`、`00:05.0` | 分别使用 file 3、4 |

每个设备具有独立 TDI、设备身份和 DMA 状态。这里的 file 编号沿用实现日志中的页索引；IID 的命名空间属于各自的 interrupt file，因此不同 file 可以使用相同 IID。

也可选择较小拓扑，将上述命令末尾的模式替换为：

| 模式 | 验证拓扑 |
| --- | --- |
| `nic-direct` | 一个 TVM、一个 vCPU、一张 e1000e |
| `multi-direct` | 一个 TVM、一个 vCPU、e1000e + EDU，独立停止设备 |
| `multicvm-direct` | 两个 TVM，各一个 vCPU，分别直通 e1000e 和 EDU |
| `multicvm-nic-pair` | 两个 TVM，各一个 vCPU、两张 e1000e |
| `multicvm-smp-nic-pair` | 两个 TVM，各两个 vCPU、两张 e1000e |

`TEST_LOG` 可指定串口日志路径，`ROOTFS_IMG` 可指定 Host 镜像。包装脚本同时检查超时、退出码、Host PASS 标记及模式对应的绑定、撤销和中断证据；仅有 QEMU 正常退出不足以判定通过。

需要交互式启动时，可执行 `bash boot-host-os.sh`，然后在 Host shell 中执行 `./run-guest-os.sh`。默认交互配置不附加直通设备，也不启用可信 I/O 模式。`GUEST_CPUS` 设置嵌套 Guest 的 vCPU 数，不代表增加外层物理 hart。

## 验证

完成默认目录构建后，运行包含多 vCPU 拓扑的统一回归：

```bash
python3 scripts/tests/secure-multicvm-test.py --with-smp \
    --out /tmp/xs-cvm-validation \
    --rootfs "$PWD/rootfs_kvm_riscv64.img" \
    --manifest "$PWD/rootfs_kvm_riscv64.img.manifest"
```

入口见 [secure-multicvm-test.py](scripts/tests/secure-multicvm-test.py)，覆盖以下八组：

- 生产函数单元验证：私有状态、内存归属、IMSIC、MMIO、生命周期等。
- 固件、可信 I/O 控制与 QEMU bitmap 三组底层验证。
- 双 TVM 四网卡、双 TVM 混合设备、单 TVM 双设备、双 TVM 多 vCPU 四网卡四组端到端验证。

结果输出到指定目录的 `validation.json`、分组日志和串口日志。回归先核对 manifest 与当前产物，结束时检查测试期间产物是否变化。

已有完整回归记录通过上述八组。多 vCPU 四网卡测试验证了实际 ICMP 收发、CPU0/CPU1 的目标中断处理、注入允许/拒绝及设备 IID 冒充拒绝，并验证一个 TVM 销毁后另一个继续收发，最终回收全部设备 TDI。

这些测试按网卡依次进行网络验证，尚不能作为四张网卡持续并发 DMA 压力或性能结论。当前工作副本是否通过，应以本次生成的结果和日志为准。

构建与日志处理的轻量回归不需要 RISC-V 工具链或 QEMU：

```bash
bash scripts/tests/build-and-log-test.sh
```

## 当前边界

- **平台范围**：RV64 TCG、单 hart、固定可信 I/O 平台；推荐配置为 1 GiB 外层 RAM、7 个 IMSIC Guest files、8 个可信 IOMMU slots。当前设备路径针对 root bus 上的 e1000e/EDU，未覆盖 PCIe 桥或 Switch 拓扑。
- **vCPU 与中断**：代码允许每 TVM 最多 4 个 vCPU，端到端验证覆盖每 TVM 2 个；可运行组合还受 IMSIC file 总量限制。file 固定保留到销毁，尚不支持 COVI 动态 unbind/rebind、多 hart 迁移和嵌套 IMSIC。
- **DMA 授权粒度**：当前私有 DMA 授权保持到 TDI STOP；普通驱动的 DMA unmap/free 不代表 Monitor 已逐页撤销授权。
- **标准覆盖**：实现部分 COVG/COVH/COVI 语义；TVM 创建和 RUN 仍使用原型自定义 ABI，尚未完成标准一致性验证。
- **硬件与协议**：bitmap、设备访问约束和可信资源保护由修改后的 QEMU 模拟；尚无真实硬件验证、设备认证与证明链、SPDM/TDISP/IDE。
- **未覆盖验证**：长期队列回绕压力、同一设备跨 TVM 重新分配的完整端到端验证，以及多 hart 扩展仍需补充。

仓库还保留早期 VFIO/MSI/PRI 实验入口。评估当前安全 I/O 原型时，请使用本文带 `HOST_COVE_IO_TRUSTED=1` 的通路和对应回归结果。
