# 安全 IO 设备的中断处理机制验证报告

验证日期：2026-07-10  
验证主题：安全 IO 设备的中断处理机制实现  
验证范围：COVE-IO 原型中的 VFIO PCI INTx/MSI/MSI-X IRQ 授权、MSI/MSI-X 动态 TDI IRQ range 绑定、PCI-RID device id 约束、AIA IMSIC target-vCPU 和 IMSIC interrupt ID 约束、RISC-V IOMMU COVE-IO isolated MSI remap、MRIF posted-interrupt 原型、retarget/pending-blocked 行为，以及负向 probe。

本报告不覆盖绑定阶段的 DMA/MMIO 身份建立，也不覆盖解绑阶段的完整 teardown；这里关注的问题是：设备绑定 CVM 后，设备中断是否只能投递到这个 CVM 授权的 GSI、PCI requester、target vCPU 和 IMSIC interrupt ID。

## 1. 验证目标

本次验证要证明：

1. CVM VFIO PCI 设备不再隐藏 MSI/MSI-X capability，guest 启用 MSI/MSI-X 后可以触发动态 IRQ 授权。
2. kvmtool 能把 guest 配置出来的 MSI/MSI-X vector 解析为 GSI、target vCPU、IMSIC interrupt ID，并写入 COVE-IO TDI IRQ authorization。
3. VFIO MSI/MSI-X route 携带 `KVM_MSI_VALID_DEVID`，Linux/KVM 能把 route devid 转换成 COVE-IO PCI-RID `device_id`。
4. Linux/KVM 在注入 MSI 前校验 GSI、PCI-RID device id、target vCPU 和 IMSIC IID；任一项不匹配则拒绝。
5. RISC-V IOMMU COVE-IO isolated MSI remap 被启用，VFIO 使用该路径而不是 `allow_unsafe_interrupts`。
6. MRIF posted-interrupt 原型能为授权设备安装 per-device MRIF PTE；retarget 时旧 vCPU 目标撤销，新 vCPU 目标授权；如果新 vCPU IMSIC VS-file 尚未可用，则 pending 状态必须阻断 MSI。

## 2. 关键实现路径

### 2.1 kvmtool 动态 IRQ 授权

- `kvmtool/vfio/pci.c` 维护 VFIO PCI MSI/MSI-X capability，并在 guest 写 MSI/MSI-X
  配置时同步 COVE-IO IRQ 授权。
- 当 guest enable MSI/MSI-X 时：
  - `vfio_pci_begin_cove_io_msi_irq()` 清除旧 INTx/旧 MSI 授权；
  - `vfio_pci_cove_io_msi_target_vcpu()` 通过 RISC-V AIA MSI address 解析 target vCPU；
  - `vfio_pci_bind_cove_io_msi_irq()` 将 GSI、target vCPU、MSI data 作为 IMSIC IID 绑定；
  - `vfio_pci_bind_cove_io_irq_target()` 调用
    `cove_io__bind_irq_target_iid_device()`，把 `device_id`、GSI、vCPU、IID 写入 TDI。
- MSI/MSI-X mask 会调用 `vfio_pci_unbind_cove_io_msi_irq_range()` 撤销对应 vector；
  MSI/MSI-X disable 会撤销动态 MSI 授权，并在可用时回退到 INTx。
- `kvmtool/irq.c` 中 `irq__add_msix_route_devid()` 为 VFIO MSI/MSI-X route 设置
  `KVM_MSI_VALID_DEVID`，route devid 使用物理 PCI `(segment << 16) | requester-id`。

### 2.2 Linux/KVM 注入前授权检查

- `linux/arch/riscv/kvm/vm.c` 中 `kvm_set_msi()` 在 CVM 模式下：
  - 从 MSI address 解析 target vCPU；
  - 从 route devid 生成 COVE-IO PCI-RID `device_id`；
  - 调用 `kvm_riscv_cove_io_irq_target_device_allowed()` 校验 GSI、vCPU、IID、device id；
  - 校验失败返回 `-EPERM`，不会调用 AIA IMSIC 注入。
- `linux/arch/riscv/kvm/vcpu.c` 中
  `kvm_riscv_cove_io_irq_target_device_allowed()` 通过 SBI 调用 OpenSBI
  `COVE_IO_TDI_FIND_IRQ`，只接受 STARTED TDI 的授权结果。

### 2.3 OpenSBI IRQ range 授权

- OpenSBI TDI 中保存多条 IRQ ranges，每条 range 记录：
  - IRQ/GSI 范围
  - target vCPU
  - IMSIC interrupt ID
  - 设备 `device_id`
- `COVE_IO_TDI_IRQ_BIND` 支持在 `BOUND` 或 `STARTED` TDI 上追加 IRQ range。
- `COVE_IO_TDI_FIND_IRQ` 要求 owner CVM、IRQ/GSI、target vCPU、IMSIC IID、
  device id 同时匹配。
- `COVE_IO_TDI_IRQ_UNBIND` 和 `STOP` 会删除 IRQ ranges，解绑后旧中断目标不能继续命中。

### 2.4 RISC-V IOMMU MSI/MRIF 路径

- `linux/drivers/iommu/riscv/iommu.c` 提供私有 COVE-IO isolated MSI 路径：
  `iommu.cove_io_isolated_msi=1` 初始化 basic-mode MSI remap table。
- VFIO type1/iommufd 只在 IOMMU driver 明确报告 COVE-IO isolated MSI 时接受该路径；
  否则不依赖 generic isolated MSI，也不打开 `allow_unsafe_interrupts`。
- `iommu.cove_io_mrif=1` 启用 MRIF posted-interrupt 原型：
  - `riscv_iommu_cove_io_mrif_bind()` 为授权的 device id、vCPU、IID 建立 per-device
    MRIF remap；
  - `kvm_riscv_aia_imsic_mrif_info()` 提供 vCPU software MRIF backing 和 notice MSI
    target；
  - 如果 vCPU VS-file 尚未可用，IOMMU driver 清空该设备 MSI PTE table，并打印
    `MSI blocked until IMSIC VS-file is available`；
  - retarget 时替换旧 vCPU 目标，并释放旧 MRIF user 引用。

## 3. 验证方案

| 编号 | 验证点 | 实验方式 | 通过标准 |
| --- | --- | --- | --- |
| IRQ-01 | VFIO MSI/MSI-X 不被隐藏，guest enable 后动态绑定 IRQ | `HOST_COVE_IO_TEST=vfio-msi` | 出现 `COVE-IO dynamic MSI/MSI-X IRQ bind`，且记录非零 IID |
| IRQ-02 | 中断注入受 GSI、device id、target vCPU、IID 共同约束 | `vfio-msi` deterministic probe | 正向 allow；wrong-device、wrong-vCPU、wrong-IID、unbound 均 deny |
| IRQ-03 | VFIO 不走 unsafe interrupt，使用 COVE-IO isolated MSI | `vfio-msi` | 日志出现 COVE-IO MSI remap 和 VFIO consume 日志，脚本未发现 `allow_unsafe_interrupts` |
| IRQ-04 | MRIF posted-interrupt per-device PTE 可安装 | `vfio-msi` | 出现 `COVE-IO experimental MRIF remap installed` |
| IRQ-05 | MRIF retarget 撤销旧 vCPU、授权新 vCPU | `vfio-msi-retarget-stress` | old target deny、新 target allow，出现 `COVE-IO MRIF remap retarget` |
| IRQ-06 | pending target 不泄露到旧目标 | `vfio-msi-retarget-stress` | pending 状态打印 `MSI blocked until IMSIC VS-file is available` |
| IRQ-07 | 自动化入口可复现 | host autorun | 串口出现 `COVE-IO host autorun: PASS ...` |

## 4. 实验环境

- Host QEMU machine：RISC-V `virt`，AIA IMSIC enabled。
- IOMMU：QEMU 模拟 `riscv-iommu-pci`，启用 `iommu.cove_io_isolated_msi=1` 和
  `iommu.cove_io_mrif=1`。
- 测试设备：QEMU `edu` PCI device，作为 VFIO PCI 直通设备。
- Guest/CVM 启动工具：kvmtool `lkvm-static --cvm`。
- Nested guest autorun：`GUEST_COVE_IO_AUTORUN=edu-msi`，在 guest 内配置 `edu`
  设备 MSI capability，写入 IMSIC MSI address 和非零 MSI data。
- Kernel/OpenSBI/kvmtool：当前 xs-cvm 工作树构建产物。

## 5. 实验记录与结果

### 5.1 IRQ-01/IRQ-02/IRQ-03/IRQ-04：基础 VFIO MSI/MRIF 中断授权

命令：

```sh
HOST_RISCV_IOMMU=1 \
HOST_PCI_TEST_DEVICE='edu,dma_mask=0xffffffff' \
HOST_COVE_IO_TEST=vfio-msi \
HOST_COVE_IO_TEST_WAIT_SECS=90 \
./boot-host-os.sh
```

关键观测：

- Host kernel command line 包含：
  `iommu.cove_io_isolated_msi=1 iommu.cove_io_mrif=1`
- RISC-V IOMMU 初始化 COVE-IO MSI remap：

```text
iommu_pci 0000:00:01.0: COVE-IO experimental MSI basic-mode remap enabled: entries=1 table_size=4096 pattern=0x28000 mask=0x0
```

- VFIO 使用 COVE-IO RISC-V IOMMU MSI remap，而不是 unsafe interrupt：

```text
vfio_iommu_type1_attach_group: using experimental COVE-IO RISC-V IOMMU MSI remap instead of generic isolated MSI
```

- probe 阶段曾出现 pending MRIF，且 pending 时明确阻断 MSI：

```text
vfio-pci 0000:00:02.0: COVE-IO experimental MRIF remap pending: device_id=0x200000000000010 vcpu=0 iid=5; MSI blocked until IMSIC VS-file is available
```

- 真实 MSI run 安装 per-device MRIF PTE：

```text
vfio-pci 0000:00:02.0: COVE-IO experimental MRIF remap installed: device_id=0x200000000000010 vcpu=0 iid=113 index=0 mrif=0x000000008a73a000 notice=0x0000000028001000
```

- 自动化结果：

```text
PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind mode=basic, device, and IID probes observed (legacy)
COVE-IO host autorun: PASS vfio-msi
```

脚本强校验内容：

- `vfio-irq-target-iid=allow`
- `vfio-irq-target-device=allow`
- `vfio-irq-target-wrong-device=deny`
- `vfio-irq-target-wrong-iid=deny`
- `vfio-irq-target-wrong-vcpu=deny`
- `vfio-irq-target-iid-unbound=deny`
- `COVE-IO dynamic MSI/MSI-X IRQ bind` 存在，且 `iid` 非零
- 未出现 `allow_unsafe_interrupts`

结果分析：

- 设备 MSI/MSI-X capability 没有被隐藏，否则 nested guest 无法 enable MSI，脚本会失败。
- kvmtool 动态绑定的 IRQ 授权包含 device id、GSI、vCPU 和 IMSIC IID。
- Linux/KVM 注入前查询 OpenSBI TDI IRQ authorization；错误 device、错误 IID、错误
  vCPU 或 unbound 状态均被拒绝。
- RISC-V IOMMU COVE-IO MSI remap 初始化成功，VFIO 明确使用该 remap path。
- MRIF PTE 安装成功，说明 posted-interrupt 原型在单 vCPU 正向路径上闭环。

### 5.2 IRQ-05/IRQ-06：MRIF retarget-stress 与 pending-blocked 验证

命令：

```sh
HOST_RISCV_IOMMU=1 \
HOST_PCI_TEST_DEVICE='edu,dma_mask=0xffffffff' \
HOST_COVE_IO_TEST=vfio-msi-retarget-stress \
HOST_COVE_IO_MRIF_RETARGET_LOOPS=8 \
HOST_COVE_IO_TEST_WAIT_SECS=180 \
./boot-host-os.sh
```

关键观测：

- 测试创建 4 vCPU CVM，并执行 8 轮 MRIF retarget probe。
- 初始 pending 目标阻断 MSI：

```text
vfio-pci 0000:00:02.0: COVE-IO experimental MRIF remap pending: device_id=0x200000000000010 vcpu=0 iid=5; MSI blocked until IMSIC VS-file is available
```

- 多轮 retarget 日志：

```text
vfio-pci 0000:00:02.0: COVE-IO MRIF remap retarget: device_id=0x200000000000010 old_vcpu=0 new_vcpu=1 iid=6
vfio-pci 0000:00:02.0: COVE-IO experimental MRIF remap pending: device_id=0x200000000000010 vcpu=1 iid=6; MSI blocked until IMSIC VS-file is available
vfio-pci 0000:00:02.0: COVE-IO MRIF remap retarget: device_id=0x200000000000010 old_vcpu=1 new_vcpu=2 iid=7
vfio-pci 0000:00:02.0: COVE-IO MRIF remap retarget: device_id=0x200000000000010 old_vcpu=2 new_vcpu=3 iid=8
vfio-pci 0000:00:02.0: COVE-IO MRIF remap retarget: device_id=0x200000000000010 old_vcpu=3 new_vcpu=0 iid=9
vfio-pci 0000:00:02.0: COVE-IO MRIF remap retarget: device_id=0x200000000000010 old_vcpu=3 new_vcpu=0 iid=13
```

- retarget-stress 后的真实 MSI run 仍能安装 MRIF PTE：

```text
vfio-pci 0000:00:02.0: COVE-IO experimental MRIF remap installed: device_id=0x200000000000010 vcpu=0 iid=113 index=0 mrif=0x000000008a814000 notice=0x0000000028001000
```

- 自动化结果：

```text
PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind mode=retarget-stress, device, and IID probes observed (legacy)
COVE-IO host autorun: PASS vfio-msi-retarget-stress
```

脚本强校验内容：

- 初始 vCPU0 target allow。
- retarget 后 old vCPU target deny。
- retarget 后 new vCPU target allow。
- IRQ unbind 后 target deny。
- 最后一轮 retarget stress 中 previous target deny。
- vCPU1 pending MRIF 时必须打印 `MSI blocked until IMSIC VS-file is available`。
- 后续单 vCPU MSI run 仍能安装 concrete per-device MRIF PTE。

结果分析：

- retarget 不只是覆盖日志路径，脚本还验证旧 vCPU 授权被撤销、新 vCPU 授权生效。
- pending target 不会继续使用旧 vCPU MRIF PTE；IOMMU driver 会阻断该设备 MSI table，
  等 VS-file 可用后再刷新安装。
- stress 场景覆盖多次 vCPU 目标替换，说明 MRIF 状态不是一次性硬编码。

## 6. 结论

第三个主题“安全 IO 设备的中断处理机制实现”已通过当前原型验证。

可以确认：

1. VFIO PCI MSI/MSI-X capability 已在 CVM 中暴露，guest enable MSI/MSI-X 后会触发
   kvmtool 动态 COVE-IO IRQ bind。
2. 动态 IRQ bind 记录 GSI、PCI-RID device id、target vCPU、IMSIC IID。
3. Linux/KVM 在 MSI 注入前解析 IMSIC target vCPU，并通过 OpenSBI `FIND_IRQ`
   校验 GSI、device id、vCPU、IID；错误 device、错误 vCPU、错误 IID、unbound 均被拒绝。
4. VFIO 中断安全不再依赖 `allow_unsafe_interrupts`；当前验证使用 RISC-V IOMMU
   COVE-IO isolated MSI remap。
5. MRIF posted-interrupt 原型已验证 per-device MRIF PTE 安装、retarget、pending-blocked
   行为。

因此，本主题可以汇报为：

> 已实现并验证安全 IO 设备的中断处理机制。设备绑定 CVM 后，VFIO MSI/MSI-X
> 中断必须经过 COVE-IO TDI IRQ authorization；KVM 注入前同时校验 GSI、
> PCI-RID device id、target vCPU 和 IMSIC interrupt ID。RISC-V IOMMU 的
> COVE-IO isolated MSI/MRIF 路径提供设备侧 MSI target 约束，retarget 时旧
> vCPU 授权被撤销，新 target pending 时 MSI 被阻断，避免中断继续投递到旧目标。
