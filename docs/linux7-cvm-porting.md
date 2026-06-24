# Linux 7.0 CVM porting notes

Goal: move the xs-cvm confidential-VM KVM extensions from the bundled Linux
6.6 tree to the external Linux 7.0 tree while keeping the userspace/kernel/SBI
ABI consistent.

## Build selection

The xs-cvm build scripts support these environment variables:

- `LINUX_DIR`: Linux source tree. Defaults to `linux`.
- `BUILD_DIR`: kernel output directory. Defaults to `build-riscv64`.
- `ARCH`, `RISCV`, `CROSS_COMPILE`: respected when already exported.

Example for the external Linux 7.0 tree:

```sh
export PATH=/nfs/home/liyunxiao/Desktop/riscv/bin:$PATH
export ARCH=riscv
export RISCV=/nfs/home/liyunxiao/Desktop/riscv
export CROSS_COMPILE=$RISCV/bin/riscv64-unknown-linux-gnu-
LINUX_DIR=/nfs/home/liyunxiao/Documents/linux BUILD_DIR=build-riscv64-v7 ./build-host.sh
```

## Private KVM ioctl ABI

The old xs-cvm Linux 6.6 tree used:

- `KVM_LOAD_FILE`: `0x49`
- `KVM_SET_SWIOTLB`: `0x53`

These conflict with Linux 7.0:

- `0x49`: `KVM_SET_USER_MEMORY_REGION2`
- `0x53`: `KVM_S390_KEYOP`

The xs-cvm private ioctl numbers are now reserved as:

- `KVM_LOAD_FILE`: `0xf0`
- `KVM_SET_SWIOTLB`: `0xf1`

Keep these definitions synchronized in:

- `linux/include/uapi/linux/kvm.h`
- `/nfs/home/liyunxiao/Documents/linux/include/uapi/linux/kvm.h`
- `kvmtool/include/linux/kvm.h`

## Linux 6.6 CVM source areas to port

Port the changes by behavior, not by whole-file replacement. Linux 7.0's
RISC-V KVM path is different from the bundled 6.6 tree.

- `include/linux/kvm_host.h`
  - `struct kvm::cmode`
- `include/uapi/linux/kvm.h`
  - `struct load_file`
  - `struct swiotlb`
  - `KVM_LOAD_FILE`
  - `KVM_SET_SWIOTLB`
- `arch/riscv/include/cvm/iie-cvm-sbi.h`
  - `SBI_EXT_CVM`
  - CVM SBI function IDs
  - CVM parameter structs and memory-pool constants
- `virt/kvm/kvm_main.c`
  - VM `cmode` initialization from `KVM_CREATE_VM` type
  - CVM create/root-page-table SBI calls
  - `KVM_LOAD_FILE` handling
  - CVM confidential-memory pool init/refill helpers
- `arch/riscv/kvm/vm.c`
  - CVM destroy/recycle path
- `arch/riscv/kvm/vcpu.c`
  - CVM vCPU create SBI call
  - CVM run path using `SBI_EXT_CVM_RUN_VCPU`
  - SWIOTLB tracking and `KVM_SET_SWIOTLB`

## Userspace areas to keep in sync

- `kvmtool/riscv/include/kvm/kvm-arch.h`
  - `CVM_VM_TYPE == SBI_EXT_CVM`
- `kvmtool/builtin-run.c`
  - `--cvm`
- `kvmtool/kvm.c`
  - VM type selection
- `kvmtool/riscv/kvm.c`
  - CVM kernel/initrd load through `KVM_LOAD_FILE`
- `kvmtool/riscv/fdt.c`
  - CVM FDT load and SWIOTLB setup

## Next porting checkpoint

After phase 1-3, the next implementation step is to add the CVM kernel-side
logic to Linux 7.0 in small patches:

1. Add CVM headers and `struct kvm::cmode`.
2. Add Linux 7.0 handlers for `KVM_LOAD_FILE` and `KVM_SET_SWIOTLB`.
3. Add VM create/destroy CVM lifecycle hooks.
4. Rework the Linux 7.0 RISC-V vCPU run path for `cmode`, preserving the
   non-CVM Linux 7.0 path.
