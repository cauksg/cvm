# 安全 IO 设备与机密虚拟机绑定机制验证报告

验证日期：2026-07-10  
验证主题：安全 IO 设备与机密虚拟机绑定机制实现  
验证范围：COVE-IO 原型中的 VFIO PCI 设备绑定、TDI 创建与启动、PCI-RID 设备身份校验、IOMMU group/fwspec 校验、MMIO/DMA 授权范围建立，以及绑定后 RISC-V IOMMU fault recovery 的正向/负向可观测结果。

本报告不覆盖解绑机制细节，也不覆盖中断投递安全闭环；这两部分分别留给后续两个主题。实验中出现的 teardown/post-exit 检查只作为辅助观测，不作为本主题核心结论。

## 1. 验证目标

本次验证要证明：

1. VFIO PCI 设备绑定到 CVM 时，系统会为该设备创建独立 COVE-IO TDI，并将 TDI 绑定到目标 CVM。
2. 绑定信息包含设备 MMIO 范围、VFIO restricted DMA 范围、PCI-RID `device_id`、VFIO IOMMU group。
3. Linux/KVM 在转发 TDI DMA_MAP 前校验 PCI requester ID、IOMMU group 和 IOMMU fwspec，防止 userspace 伪造设备身份。
4. OpenSBI 只允许 STARTED 且属于该 CVM 的 TDI 命中 MMIO/DMA 查询，DMA 查询还必须匹配 `device_id` 和 GPA/IOVA 范围。
5. 绑定后的授权 DMA fault 可以恢复映射；错误设备身份、错误 RID 或未知设备类型不会被授权。
6. 多 VFIO 设备同时绑定同一 CVM 时，每个设备应保持独立 requester identity 和 per-device IOAS/HWPT 绑定策略。

## 2. 关键实现路径

### 2.1 kvmtool 创建并启动 TDI

- `kvmtool/cove-io.c` 中 `cove_io__start_tdi()` 按顺序执行：
  `REGISTER -> ADD_MMIO -> BIND -> DMA_MAP -> IRQ_BIND -> ACCEPT_START`。
- VFIO PCI 绑定入口在 `kvmtool/vfio/pci.c` 的
  `vfio_pci_start_cove_io_tdi()`。它为 VFIO PCI 设备生成 COVE-IO
  PCI-RID `device_id`，传递 `segment`、`requester-id`、`iommu_group`、
  MMIO BAR 范围和 VFIO DMA window，并打印：
  `COVE-IO vfio-pci TDI ... device_id=... rid=... iommu_group=...`.
- VFIO probe 入口 `vfio_pci_probe_cove_io_tdi()` 对绑定后的 TDI 做
  `vfio-mmio`、`vfio-dma`、`vfio-dma-wrong-device`、
  `vfio-dma-wrong-rid`、`vfio-dma-unknown-type` 等查询。

### 2.2 Linux/KVM 校验设备身份

- `linux/virt/kvm/kvm_main.c` 将私有 `KVM_COVE_IO_TDI_OP` 分发到
  RISC-V KVM 实现。
- `linux/arch/riscv/kvm/vcpu.c` 中 `kvm_vm_ioctl_cove_io_tdi()` 在
  `DMA_MAP` 路径校验 PCI-RID `device_id` 和 userspace 传入的
  `iommu_group`。
- `kvm_riscv_cove_io_validate_pci_group()` 通过 kernel PCI core 找到
  对应 PCI device，要求：
  - kernel IOMMU group id 等于 userspace 上报 group；
  - IOMMU fwspec ids 中包含同一个 requester ID。
- `kvm_riscv_cove_io_dma_allowed()` 和
  `kvm_riscv_cove_io_iommu_fault_check()` 将后续 DMA/fault 查询转换为
  OpenSBI TDI lookup。

### 2.3 OpenSBI 保存绑定状态并执行授权

- OpenSBI 的 TDI 状态机定义在 `opensbi/include/sbi/sbi_cvm.h`，状态包括
  `FREE`、`REGISTERED`、`BOUND`、`STARTED`、`STOPPING`。
- `opensbi/lib/sbi/sbi_cvm.c` 中 `cove_io_tdi_op()` 执行 TDI 操作：
  - `REGISTER` 创建/重置 TDI；
  - `BIND` 绑定 CVM owner；
  - `DMA_MAP` 写入 `device_id` 和 DMA 范围；
  - `ACCEPT_START` 将 TDI 置为 `STARTED`；
  - `FIND_MMIO` / `FIND_DMA` 只查询 STARTED 且 owner 匹配的 TDI。
- `cove_io_find_started_dma_tdi()` 要求 device id 合法、TDI owner 匹配、
  `device_id` 匹配且 GPA/IOVA 落在授权 DMA 范围内。

## 3. 验证方案

| 编号 | 验证点 | 实验方式 | 通过标准 |
| --- | --- | --- | --- |
| BIND-01 | 单 VFIO PCI 设备可以绑定到 CVM，并建立 VFIO DMA window | `HOST_COVE_IO_TEST=vfio-lazy` | VFIO 设备进入 vfio-pci，RISC-V IOMMU fault 对 RID `0x10` 授权恢复，脚本 PASS |
| BIND-02 | 绑定后 DMA 授权按 requester ID 和 IOVA 生效 | `vfio-lazy` 中 debug translation | 第一次 translation fault，恢复后第二次成功；相邻页同样可恢复 |
| BIND-03 | 错误设备身份不能命中绑定 TDI | `HOST_COVE_IO_TEST=vfio-msi` 的 deterministic probe | probe 全部通过；错误 RID/unknown device type 预期为 deny |
| BIND-04 | 多 VFIO PCI 设备绑定时保持独立设备身份和 IOAS/HWPT 策略 | `HOST_COVE_IO_TEST=vfio-multi` | 两个不同 RID/group 都创建 TDI/IOAS，各自 fault recovery PASS |
| BIND-05 | 自动化入口可复现 | host autorun | host 串口出现 `COVE-IO host autorun: PASS ...` |

## 4. 实验环境

- Host QEMU machine：RISC-V `virt`，AIA IMSIC enabled。
- IOMMU：QEMU 模拟 `riscv-iommu-pci`。
- 测试设备：QEMU `edu` PCI device。
- Guest/CVM 启动工具：kvmtool `lkvm-static --cvm`。
- Kernel/OpenSBI/kvmtool：当前 xs-cvm 工作树构建产物。
- 说明：`vfio-lazy` / `vfio-multi` 为绑定和 DMA/fault 测试，会临时允许
  VFIO unsafe interrupt override；本主题不据此判断中断安全，第三次汇报单独验证中断机制。

## 5. 实验记录与结果

### 5.1 BIND-01/BIND-02：单设备绑定与 DMA 授权恢复

命令：

```sh
HOST_RISCV_IOMMU=1 \
HOST_PCI_TEST_DEVICE='edu,dma_mask=0xffffffffffffffff' \
HOST_COVE_IO_TEST=vfio-lazy \
HOST_COVE_IO_TEST_WAIT_SECS=90 \
./boot-host-os.sh
```

关键观测：

- Host 识别 `0000:00:02.0 [1234:11e8]` 为 VFIO 测试设备。
- 设备所在 IOMMU group 为 `1`。
- VFIO 绑定日志：
  `bound 0000:00:02.0 (0x1234:0x11e8) to vfio-pci`,
  `iommu_group=1`,
  `kvmtool argument: --vfio-pci 0000:00:02.0`.
- RISC-V IOMMU fault recovery 日志：
  - `Fault 15 devid: 0x10 iotval: 82d00000 ... cove_io=allow-vfio-map ... rc=0`
  - `Fault 15 devid: 0x10 iotval: 82d01000 ... cove_io=allow-vfio-map ... rc=0`
- 自动化脚本结果：

```text
PASS: VFIO lazy fault recovery repeated, adjacent-page, and teardown checks passed (legacy)
first=0x0000000000003C01 second=0x0000000022479400 third=0x0000000000003C01 fourth=0x0000000022EFEC00 post_exit=0x0000000020B40000
COVE-IO host autorun: PASS vfio-lazy
```

结果分析：

- `devid=0x10` 是 `0000:00:02.0` 的 PCI requester ID。
- 第一次 translation 返回 fault，说明 lazy 模式没有提前安装 IOMMU PTE。
- KVM/OpenSBI 根据绑定的 PCI-RID `device_id` 和 DMA window 授权后，VFIO
  fault recovery 安装 4K 映射；第二次 translation 成功。
- 相邻页也按相同机制恢复，说明绑定的 DMA 范围不是单点硬编码。
- 实验通过 BIND-01 和 BIND-02。

### 5.2 BIND-04：双设备绑定与 per-device IOAS/HWPT 隔离

命令：

```sh
HOST_RISCV_IOMMU=1 \
HOST_PCI_TEST_DEVICES='edu,dma_mask=0xffffffffffffffff edu,dma_mask=0xffffffffffffffff' \
HOST_COVE_IO_TEST=vfio-multi \
HOST_COVE_IO_TEST_WAIT_SECS=90 \
./boot-host-os.sh
```

关键观测：

- Host 识别两个 VFIO 测试设备：
  - `0000:00:02.0`，IOMMU group `1`，RID `0x10`；
  - `0000:00:03.0`，IOMMU group `2`，RID `0x18`。
- RISC-V IOMMU fault recovery 日志：
  - `Fault 15 devid: 0x10 iotval: 82d00000 ... cove_io=allow-vfio-map ... rc=0`
  - `Fault 15 devid: 0x18 iotval: 82d00000 ... cove_io=allow-vfio-map ... rc=0`
- 自动化脚本结果：

```text
PASS: VFIO multi-device per-IOAS lazy fault recovery and teardown checks passed (iommufd)
dev1=0000:00:02.0 rid=0x10 group=1 first=0x0000000000003C01 second=0x0000000022515000 post=0x0000000020B40000
dev2=0000:00:03.0 rid=0x18 group=2 first=0x0000000000003C01 second=0x0000000022515000 post=0x0000000020B40000
COVE-IO host autorun: PASS vfio-multi
```

结果分析：

- 两个设备具有不同 RID 和不同 IOMMU group，绑定时不能共享同一个
  VFIO group identity。
- iommufd 路径为 COVE-IO VFIO 设备使用 per-device IOAS/HWPT 策略。
- 两个 RID 分别触发并恢复 fault，证明 RISC-V IOMMU fault path 使用
  requester ID 参与 COVE-IO 绑定授权，而不是只根据 IOVA 判断。
- 实验通过 BIND-04。

### 5.3 BIND-03：错误设备身份负向 probe

命令：

```sh
HOST_RISCV_IOMMU=1 \
HOST_PCI_TEST_DEVICE='edu,dma_mask=0xffffffff' \
HOST_COVE_IO_TEST=vfio-msi \
HOST_COVE_IO_TEST_WAIT_SECS=90 \
./boot-host-os.sh
```

该脚本在真实 MSI 运行前会先执行 deterministic VFIO COVE-IO probe。
绑定主题只使用该 probe 阶段的结论；中断安全结论留给第三个主题。

关键观测：

```text
PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind mode=basic, device, and IID probes observed (legacy)
COVE-IO host autorun: PASS vfio-msi
```

结果分析：

- `vfio-msi` 脚本的前置 probe 调用 kvmtool 的
  `vfio_pci_probe_cove_io_tdi()`。
- 该 probe 包含：
  - `vfio-mmio=allow`
  - `vfio-dma=allow`
  - `vfio-dma-wrong-device=deny`
  - `vfio-dma-wrong-rid=deny`
  - `vfio-dma-unknown-type=deny`
- kvmtool 的 `cove_io__expect_probe()` 对每个结果做强校验；任一负向项
  未被 deny 都会导致脚本失败。
- 脚本 PASS 说明绑定后的 TDI 查询不是单纯按 GPA/IOVA 放行，而是同时受
  PCI-RID `device_id` 和设备类型约束。
- 实验通过 BIND-03。

## 6. 结论

第一个主题“安全 IO 设备与机密虚拟机绑定机制实现”已通过当前原型验证。

可以确认：

1. VFIO PCI 设备绑定 CVM 时会创建对应 COVE-IO TDI，并进入可查询的
   STARTED 状态。
2. 绑定信息包含 PCI-RID `device_id`、IOMMU group、MMIO BAR 范围和
   VFIO restricted DMA window。
3. Linux/KVM 在 TDI DMA_MAP 入口校验 PCI requester ID、IOMMU group 和
   fwspec，防止 userspace 伪造 PCI-RID 或 group。
4. OpenSBI 的 TDI lookup 只允许 owner CVM 的 STARTED TDI 命中授权范围。
5. RISC-V IOMMU fault recovery 已证明绑定后的授权 DMA 可以按 requester
   ID 和 IOVA 范围恢复映射。
6. 多 VFIO 设备绑定同一 CVM 时，系统可以基于不同 RID/IOMMU group 建立
   独立绑定关系，并通过 iommufd per-device IOAS/HWPT 策略隔离。
7. 错误设备、错误 RID、未知设备类型不能通过绑定授权查询。

因此，本主题可以汇报为：

> 已实现并验证安全 IO 设备与机密虚拟机的绑定机制。原型系统能够在 VFIO
> PCI 设备绑定 CVM 时创建 TDI，记录 PCI-RID 设备身份、IOMMU group、
> MMIO/DMA 授权范围，并在 Linux/KVM 与 OpenSBI 两层执行身份和范围校验；
> 绑定后的 RISC-V IOMMU fault recovery 仅对匹配 requester ID 和授权范围的
> DMA 请求安装映射。

## 7. 当前限制

- 当前验证基于 QEMU 模拟 `riscv-iommu-pci` 和 QEMU `edu` PCI 设备，不是物理
  RISC-V IOMMU 或真实 PCIe 设备。
- 当前是 COVE-IO 私有原型 ABI，不是 upstream 稳定 ABI。
- 本主题不包含设备加密、认证、attestation。
- 本主题不评价中断安全闭环；MSI/MRIF/IMSIC 中断处理应在第三个主题单独汇报。
- 本主题不展开解绑状态机；STOP、DMA_UNMAP、RECLAIM_MMIO、UNBIND 等解绑
  流程应在第二个主题单独汇报。

