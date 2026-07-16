# 安全 IO 设备与机密虚拟机解绑机制验证报告

验证日期：2026-07-10  
验证主题：安全 IO 设备与机密虚拟机解绑机制实现  
验证范围：COVE-IO 原型中的 VFIO PCI TDI 停止、IRQ 授权撤销、DMA 授权撤销、MMIO 授权回收、TDI 与 CVM owner 解绑、VM destroy fallback 清理，以及解绑后 RISC-V IOMMU translation 不再保持 CVM 成功映射的可观测结果。

本报告不覆盖绑定阶段的身份建立细节，也不覆盖中断动态投递安全闭环；这两部分分别由第一个和第三个主题单独汇报。这里关注的问题是：设备从 CVM 解绑后，原先对该 CVM 生效的 MMIO、DMA、IRQ 授权是否被撤销，后续请求是否不能继续命中旧 TDI。

## 1. 验证目标

本次验证要证明：

1. kvmtool 在 VFIO PCI 设备 teardown 和全局 VM exit 时会显式执行 TDI 解绑序列。
2. TDI 解绑序列包含 `STOP -> IRQ_UNBIND -> DMA_UNMAP -> RECLAIM_MMIO -> UNBIND`，而不是只关闭 userspace 设备 fd。
3. Linux/KVM 在 VM destroy fallback 路径也会扫描并清理 CVM 下的 TDI，避免 userspace 异常退出留下 monitor 授权状态。
4. OpenSBI 在 `STOP` 阶段清除 MMIO、DMA、device id 和 IRQ ranges，并且只允许 owner CVM 对 BOUND/STOPPING TDI 做清理。
5. VFIO/IOMMU 层的 DMA 映射会随设备/VM teardown 撤销；解绑后同一个 RID/IOVA 不应继续返回之前的 CVM 成功 translation。
6. 多 VFIO 设备场景中，不同 RID/IOMMU group 的 per-device IOAS/HWPT 映射都能在 teardown 后撤销。

## 2. 关键实现路径

### 2.1 kvmtool 显式 teardown

- `kvmtool/cove-io.c` 中 `cove_io__teardown_tdi()` 先查询 `GET_STATE`。
- 如果 TDI 处于 `STARTED`，先执行 `STOP`，再进入 `STOPPING` 清理。
- 对 `BOUND` 或 `STOPPING` 状态的 TDI，依次执行：
  `IRQ_UNBIND -> DMA_UNMAP -> RECLAIM_MMIO -> UNBIND`。
- `kvmtool/vfio/pci.c` 中 `vfio_pci_teardown_device()` 在 CVM 模式下调用
  `cove_io__teardown_tdi()`，然后再 unmap VFIO BAR、注销 device、释放 MSI/MSI-X
  数据结构。
- `kvmtool/kvm.c` 的全局 VM exit 路径也会遍历 TDI 并调用同一个 teardown helper。

### 2.2 Linux/KVM fallback 清理

- `linux/arch/riscv/kvm/vcpu.c` 中 `kvm_riscv_cove_io_destroy_vm()` 在 CVM
  销毁时执行兜底清理。
- 该函数先调用 `riscv_iommu_cove_io_mrif_unbind_vm(kvm)` 清理该 VM 关联的
  MRIF posted-interrupt 状态。
- 然后扫描 `1..KVM_COVE_IO_MAX_TDIS`，对属于该 CVM 的 TDI 执行：
  `GET_STATE -> STOP -> IRQ_UNBIND -> DMA_UNMAP -> RECLAIM_MMIO -> UNBIND`。
- 这条路径覆盖 userspace 没有正常完成 teardown 的情况。

### 2.3 OpenSBI 状态机清理

- `opensbi/lib/sbi/sbi_cvm.c` 中 `COVE_IO_TDI_STOP` 将 TDI 置为
  `STOPPING`，并清除：
  - `mmio_gpa/mmio_size`
  - `dma_gpa/dma_size`
  - `device_id`
  - 所有 IRQ ranges
- `COVE_IO_TDI_IRQ_UNBIND` 在 `BOUND`、`STARTED`、`STOPPING` 状态下可执行，
  可清除指定 IRQ range 或全部 IRQ ranges。
- `COVE_IO_TDI_DMA_UNMAP` 清除 DMA window，并把 `device_id` 复位为
  `COVE_IO_DEVICE_ANY`。
- `COVE_IO_TDI_RECLAIM_MMIO` 清除 MMIO window。
- `COVE_IO_TDI_UNBIND` 调用 `cove_io_reset_binding()`，将 TDI 退回
  `REGISTERED` 并递增 generation。
- `FIND_MMIO`、`FIND_DMA`、`FIND_IRQ` 只会命中 STARTED 且 owner 匹配的 TDI；
  因此 STOP/UNBIND 后旧授权不能继续被查询命中。

## 3. 验证方案

| 编号 | 验证点 | 实验方式 | 通过标准 |
| --- | --- | --- | --- |
| UNBIND-01 | 单 VFIO 设备 teardown 后 DMA translation 不再保持旧成功映射 | `HOST_COVE_IO_TEST=vfio-lazy` | 脚本 PASS，`post_exit` 不等于 teardown 前成功 translation |
| UNBIND-02 | teardown 前授权 fault recovery 正常，避免误把未绑定误判为解绑成功 | `vfio-lazy` repeated/adjacent-page fault | 两页先 fault 后恢复，确认解绑前绑定确实生效 |
| UNBIND-03 | 多 VFIO 设备 teardown 后两个 RID 的映射都被撤销 | `HOST_COVE_IO_TEST=vfio-multi` | 两个设备均 PASS，两个 `post` 均不保持旧成功 translation |
| UNBIND-04 | VM destroy fallback 与 OpenSBI owner/state 约束存在 | 代码审查 + 自动化 teardown | kvmtool 与 KVM 均有显式 cleanup 序列，OpenSBI 只允许 owner 清理 |
| UNBIND-05 | 自动化入口可复现 | host autorun | 串口出现 `COVE-IO host autorun: PASS ...` |

## 4. 实验环境

- Host QEMU machine：RISC-V `virt`，AIA IMSIC enabled。
- IOMMU：QEMU 模拟 `riscv-iommu-pci`。
- 测试设备：QEMU `edu` PCI device。
- Guest/CVM 启动工具：kvmtool `lkvm-static --cvm`。
- Kernel/OpenSBI/kvmtool：当前 xs-cvm 工作树构建产物。
- 说明：`vfio-lazy` 和 `vfio-multi` 为 DMA/teardown 专项测试，会临时设置
  VFIO unsafe interrupt override。这不用于证明中断安全；中断安全由第三个主题的
  MSI/MRIF 测试单独验证。

## 5. 实验记录与结果

### 5.1 UNBIND-01/UNBIND-02：单设备解绑与 post-exit translation 检查

命令：

```sh
HOST_RISCV_IOMMU=1 \
HOST_PCI_TEST_DEVICE='edu,dma_mask=0xffffffffffffffff' \
HOST_COVE_IO_TEST=vfio-lazy \
HOST_COVE_IO_TEST_WAIT_SECS=90 \
./boot-host-os.sh
```

关键观测：

- VFIO 设备绑定：
  - `bound 0000:00:02.0 (0x1234:0x11e8) to vfio-pci`
  - `iommu_group=1`
  - `kvmtool argument: --vfio-pci 0000:00:02.0`
- teardown 前两页 lazy fault recovery：
  - `Fault 15 devid: 0x10 iotval: 82d00000 ... cove_io=allow-vfio-map phys=0x000000008bd36000 size=0x1000 rc=0`
  - `Fault 15 devid: 0x10 iotval: 82d01000 ... cove_io=allow-vfio-map phys=0x000000008bf0e000 size=0x1000 rc=0`
- OpenSBI CVM 删除日志：
  - `deleting CVM 0`
  - `Delete VCPU Successfully.`
  - `Delete CVM Successfully.`
- 自动化结果：

```text
PASS: VFIO lazy fault recovery repeated, adjacent-page, and teardown checks passed (legacy)
first=0x0000000000003C01 second=0x0000000022F4D800 third=0x0000000000003C01 fourth=0x0000000022FC3800 post_exit=0x0000000020B40000
COVE-IO host autorun: PASS vfio-lazy
```

结果分析：

- `first` 和 `third` 的低位 fault 标志为 1，说明初始 translation 确实 fault。
- `second` 和 `fourth` 变为非 fault 成功值，说明 teardown 前 COVE-IO 绑定与
  VFIO lazy recovery 已经安装了有效 IOMMU 映射。
- `post_exit=0x0000000020B40000` 与 teardown 前成功值
  `second=0x0000000022F4D800` 不同，且没有继续保持同一个 CVM 成功 translation。
- 因此单设备场景下，VM/VFIO teardown 后旧 DMA 授权映射没有继续保留。

### 5.2 UNBIND-03：双设备 per-device IOAS/HWPT 解绑

命令：

```sh
HOST_RISCV_IOMMU=1 \
HOST_PCI_TEST_DEVICES='edu,dma_mask=0xffffffffffffffff edu,dma_mask=0xffffffffffffffff' \
HOST_COVE_IO_TEST=vfio-multi \
HOST_COVE_IO_TEST_WAIT_SECS=90 \
./boot-host-os.sh
```

关键观测：

- 两个 VFIO 设备：
  - `0000:00:02.0`，RID `0x10`，IOMMU group `1`
  - `0000:00:03.0`，RID `0x18`，IOMMU group `2`
- 两个 RID 都完成 COVE-IO lazy fault recovery：
  - `Fault 15 devid: 0x10 iotval: 82d00000 ... cove_io=allow-vfio-map phys=0x00000000891e8000 size=0x1000 rc=0`
  - `Fault 15 devid: 0x18 iotval: 82d00000 ... cove_io=allow-vfio-map phys=0x00000000891e8000 size=0x1000 rc=0`
- 自动化结果：

```text
PASS: VFIO multi-device per-IOAS lazy fault recovery and teardown checks passed (iommufd)
dev1=0000:00:02.0 rid=0x10 group=1 first=0x0000000000003C01 second=0x000000002247A000 post=0x0000000020B40000
dev2=0000:00:03.0 rid=0x18 group=2 first=0x0000000000003C01 second=0x000000002247A000 post=0x0000000020B40000
COVE-IO host autorun: PASS vfio-multi
```

结果分析：

- 两个设备使用不同 RID 和不同 IOMMU group，测试覆盖 per-device IOAS/HWPT
  teardown，而不是单个设备的偶然情况。
- 两个设备在 teardown 前都从 fault 变为成功 translation，证明两条设备授权均真实生效。
- 两个设备的 `post=0x0000000020B40000` 都不再保持 teardown 前的成功 translation。
- 因此多设备场景下，两个设备的 COVE-IO DMA 授权和 IOMMU 映射都能在 teardown 后撤销。

## 6. 结论

第二个主题“安全 IO 设备与机密虚拟机解绑机制实现”已通过当前原型验证。

可以确认：

1. kvmtool 的 VFIO device teardown 和 VM exit 路径都会调用统一的
   `cove_io__teardown_tdi()`。
2. teardown 顺序覆盖 `STOP`、`IRQ_UNBIND`、`DMA_UNMAP`、`RECLAIM_MMIO`、
   `UNBIND`，对应 MMIO、DMA、IRQ 三类授权撤销。
3. Linux/KVM 的 VM destroy fallback 也会执行同等清理，并额外清理该 VM 的
   MRIF posted-interrupt 状态。
4. OpenSBI `STOP` 会清空 TDI 中的 MMIO、DMA、device id 和 IRQ ranges；
   `FIND_*` 查询只命中 STARTED TDI，因此 STOP/UNBIND 后旧授权不会继续生效。
5. 单设备和双设备自动化实验均证明：teardown 前已恢复的 DMA translation，在
   VM/VFIO teardown 后不再保持原来的 CVM 成功映射。

因此，本主题可以汇报为：

> 已实现并验证安全 IO 设备与机密虚拟机的解绑机制。原型系统在 VFIO PCI 设备
> 或 CVM 退出时，会对 TDI 执行 STOP、IRQ_UNBIND、DMA_UNMAP、RECLAIM_MMIO、
> UNBIND 的对称清理，并在 OpenSBI monitor 中撤销 MMIO/DMA/IRQ 授权状态；
> RISC-V IOMMU debug translation 实验证明，解绑后旧 DMA 映射不再继续保持。

## 7. 当前限制

- 当前验证基于 QEMU 模拟 `riscv-iommu-pci` 和 QEMU `edu` PCI 设备，不是物理
  RISC-V IOMMU 或真实 PCIe 设备。
- 当前是 COVE-IO 私有原型 ABI，不是 upstream 稳定 ABI。
- teardown 实验通过 IOMMU translation 结果验证授权映射消失，但不等价于完整资源泄漏证明。
- 本主题不证明中断安全；`vfio-lazy` 和 `vfio-multi` 允许 unsafe interrupt 只是为了专注 DMA teardown。
- 本主题不包含设备加密、认证、attestation。
