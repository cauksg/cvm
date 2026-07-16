# COVE-IO Review Notes

This change implements a COVE-IO authorization prototype for xs-cvm. It does
not implement device encryption, attestation, authentication, or a stable
upstream ABI.

## Scope

- Linux exposes the private `KVM_COVE_IO_TDI_OP` VM ioctl and uses OpenSBI TDI
  lookup results to gate COVE MMIO, DMA share, IRQ, and MSI paths.
- OpenSBI owns the in-monitor TDI table, TDI lifecycle, COVE-IO lookup helpers,
  and per-CVM cleanup.
- kvmtool creates the default/platform, PCI, virtio-mmio, and VFIO PCI TDIs
  when `--cvm` is used. It also programs the restricted DMA pool and supports
  probe-only negative tests through environment variables.
- The initramfs contains host and guest autorun hooks. Host autorun can launch
  the VFIO lazy-fault or VFIO MSI/IID regression tests from the host kernel
  command line. Guest autorun can configure the nested `edu` PCI device's MSI
  capability through PCI config space, giving the MSI/MSI-X dynamic IRQ-bind
  path deterministic runtime coverage without a full in-guest PCI driver.
- In CVM mode, kvmtool maps only the restricted DMA pool into the VFIO type1
  container, forces VFIO BARs through trapped MMIO, and exposes VFIO PCI
  MSI/MSI-X capabilities to the guest. When the nested guest enables MSI or
  MSI-X, kvmtool clears any stale INTx IRQ authorization and binds each
  unmasked MSI/MSI-X GSI as its own COVE-IO IRQ range. On RISC-V AIA, kvmtool
  also resolves the guest MSI address back to the target IMSIC/vCPU and stores
  the VFIO PCI `device_id`, target vCPU, and IMSIC interrupt ID (`msi.data`) in
  the TDI IRQ authorization. MSI/MSI-X vector masking removes the corresponding
  GSI range, and MSI/MSI-X disable rebinds the TDI to INTx when INTx is
  available or clears the MSI/MSI-X IRQ authorization when there is no INTx
  fallback.
  OpenSBI stores multiple IRQ ranges per TDI, so sparse MSI/MSI-X GSI
  allocation is not represented as an over-broad min/max range. Linux/KVM
  mirrors this by resolving MSI injection targets through the in-kernel AIA
  IMSIC addresses and rejecting CVM MSI injection if the authorized GSI, VFIO
  PCI `device_id`, target vCPU, or IMSIC interrupt ID does not match. In CVM
  mode, kvmtool programs VFIO MSI/MSI-X routes with `KVM_MSI_VALID_DEVID` using
  the physical PCI `(segment << 16) | requester-id`, and Linux/KVM converts
  that route devid back to the COVE-IO PCI-RID `device_id` before asking
  OpenSBI. If kvmtool cannot resolve the target IMSIC for a VFIO MSI/MSI-X
  vector, it leaves that vector unauthorized so the KVM injection check fails
  closed.
- VFIO/iommufd no longer need the test-only `allow_unsafe_interrupts` override
  for the QEMU `riscv-iommu-pci` MSI/MSI-X validation path. Linux has a
  private COVE-IO IOMMU hook, `iommu_group_has_cove_io_isolated_msi()`, separate
  from the generic `iommu_group_has_isolated_msi()` contract. The RISC-V IOMMU
  driver implements that hook by enabling MSI flat-table basic-mode remapping
  for the device context before VFIO attaches the group. The table is derived
  from the host IMSIC global config, sized to the IMSIC target topology, and
  also supports the single-target QEMU topology by installing one PTE that only
  accepts the IMSIC base target. VFIO type1 and iommufd accept this path only
  when the IOMMU driver explicitly reports it; otherwise they continue to fail
  closed unless the user deliberately enables `allow_unsafe_interrupts`. For
  this prototype's default VFIO eventfd/KVM-injection path, interrupt isolation
  is the combination of RISC-V IOMMU MSI target filtering plus
  kvmtool/OpenSBI/Linux KVM GSI, PCI-RID device-id, target-vCPU, and IMSIC-IID
  authorization.
  The tree also has an opt-in COVE-IO MRIF posted-interrupt prototype:
  `iommu.cove_io_mrif=1` makes Linux install a per-VFIO-PCI-device MSI PTE in
  MRIF mode after dynamic MSI/MSI-X IRQ authorization. KVM AIA exposes the
  vCPU's software MRIF backing and current notice MSI target, while the RISC-V
  IOMMU driver programs `MRIF_ADDR` plus notice `NPPN/NID` for the authorized
  device RID/vCPU/IID. KVM queues an asynchronous IOMMU MRIF refresh when an
  IMSIC VS-file target changes so the per-device notice target follows the
  bound CVM vCPU. If userspace binds a VFIO MSI/MSI-X IRQ before the target
  vCPU has an allocated IMSIC VS-file, the RISC-V IOMMU driver records the
  MRIF binding, switches the device to a private MSI PTE table, clears that
  table so MSI delivery is blocked, and lets the AIA refresh path install the
  MRIF PTE once the VS-file becomes available. This prevents a retargeted
  device from continuing to use the old vCPU's notice target while the new
  target is pending. This is still a private COVE-IO experiment, not a generic
  upstream RISC-V IOMMU interrupt-remapping ABI.
- The CVM restricted DMA area is split into a virtio sub-pool
  (`0x82c00000+1M`) and a VFIO sub-pool (`0x82d00000+1M`). The full 2M range is
  still passed to KVM as the CVM SWIOTLB range, but device TDIs and probes use
  the per-device sub-pool. This prevents a VFIO DMA lookup from accidentally
  matching a virtio DMA TDI.
- DMA TDI lookup now carries a `device_id` through kvmtool, the private KVM
  ioctl, Linux's SBI parameter block, and OpenSBI's TDI table. Probe coverage
  verifies that the same DMA GPA is denied when queried with the wrong device
  id. The runtime SWIOTLB page-fault path now resolves the fault GPA to a
  registered SWIOTLB sub-pool and passes that sub-pool's `device_id` to
  OpenSBI. `KVM_COVE_IO_DEVICE_ANY` and `KVM_COVE_IO_DEVICE_INVALID` are not
  accepted as DMA lookup wildcards. The device-id type field is now part of
  the Linux UAPI copy, kvmtool UAPI copy, and OpenSBI ABI header; OpenSBI only
  accepts known COVE-IO DMA device types.
- VFIO PCI `device_id` values are encoded as COVE-IO PCI-RID ids:
  `type | segment | requester-id`, where requester-id is the PCI BDF encoding
  `(bus << 8) | (device << 3) | function`. This is still registered by
  kvmtool, but it now has the same identity shape that a real PCI/IOMMU fault
  path should eventually provide.
- VFIO PCI `DMA_MAP` now also carries the VFIO IOMMU group id into the private
  KVM ioctl. Before forwarding the TDI operation to OpenSBI, Linux/KVM resolves
  the PCI-RID `device_id` through the kernel PCI core and verifies that the
  kernel IOMMU group id for that PCI device matches the group id reported by
  kvmtool's VFIO setup. It also verifies that the kernel IOMMU fwspec attached
  to the PCI device contains the same requester ID. This rejects a
  userspace-supplied PCI RID that does not match the kernel's PCI/IOMMU device
  model.
- SWIOTLB sub-pool registration now also carries the VFIO IOMMU group id.
  Linux/KVM applies the same PCI-RID to kernel-IOMMU-group validation before
  accepting a PCI-RID SWIOTLB sub-pool, and also requires the PCI RID to appear
  in the kernel IOMMU fwspec ids for the PCI device. This means the runtime DMA
  share path no longer relies only on an unchecked userspace-provided sub-pool
  `device_id`.
- The RISC-V IOMMU fault queue path now feeds the fault DID/RID and IOVA into
  KVM's COVE-IO authorization lookup. Linux's RISC-V IOMMU driver extracts
  `devid` and `iotval` from fault queue records, KVM matches the RID against
  registered PCI-RID SWIOTLB sub-pools, and KVM reuses the OpenSBI TDI DMA
  lookup. Authorized faults are handed to VFIO container/type1 fault recovery:
  type1 finds the lazy DMA entry, pins the faulting 4K IOVA page, updates its
  `vfio_pfn`/locked-memory accounting, and installs the IOMMU mapping through
  the existing VFIO mapping path, logging `cove_io=allow-vfio-map`. Denied
  faults still log `cove_io=deny-observe`, and faults that do not match a
  registered COVE-IO sub-pool still fall back to the normal RISC-V IOMMU fault
  log.
- The iommufd IOAS/HWPT path now has the same private lazy DMA mapping shape.
  `IOMMU_IOAS_MAP_COVE_IO_LAZY` and VFIO compat
  `VFIO_DMA_MAP_FLAG_COVE_IO_LAZY` create IOAS areas without installing domain
  PTEs up front. When VFIO is attached through iommufd, the VFIO group recovery
  hook finds the still-attached `iommufd_device`/HWPT, confirms the faulting
  device is present in the default PASID attach, splits the lazy IOAS area down
  to the faulting 4K page, pins/maps it through existing iommufd page-table
  helpers, and returns the installed host physical page. The COVE-IO policy is
  per-device IOAS/HWPT isolation, not requester-scoped recovery on a shared
  HWPT. kvmtool allocates one IOAS per COVE-IO VFIO device, switches the VFIO
  compatibility IOAS before opening each device fd, and rejects same-group
  reuse because the group compatibility path cannot safely express multiple
  private HWPTs within one VFIO IOMMU group. The kernel path also fails closed
  if a lazy range is created in a multi-domain IOAS, if a second domain is
  attached while lazy areas exist, or if recovery finds that the target
  HWPT/domain is shared by more than one attached device.
- The host kernel config now enables RISC-V IOMMU, VFIO container/type1,
  iommufd, vfio-pci, and KVM VFIO support. `boot-host-os.sh` can add QEMU
  `riscv-iommu-pci` and a test PCI device through environment variables.
- TDI teardown is now symmetric in both userspace and kernel fallback paths.
  kvmtool uses one `cove_io__teardown_tdi()` helper for VFIO device teardown and
  global VM exit; it performs `STOP`, `IRQ_UNBIND`, `DMA_UNMAP`,
  `RECLAIM_MMIO`, and `UNBIND` when the TDI is bound or stopping. Linux/KVM's
  VM destroy path performs the same explicit cleanup sequence before destroying
  the CVM. OpenSBI `STOP` clears MMIO, DMA, device id, and all IRQ ranges, and
  `IRQ_UNBIND`/`DMA_UNMAP`/`RECLAIM_MMIO` are accepted during STOPPING cleanup,
  so STOP-then-UNBIND no longer leaves stale authorization state. The OpenSBI
  lifecycle checks also reject cleanup/unregister requests against FREE or
  STARTED TDIs and require the bound owner for BOUND/STOPPING TDIs.

## Regression Commands

Build:

```sh
./build-host.sh
make -C linux O=/nfs/home/liyunxiao/xs-cvm/build-riscv64 ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- \
  drivers/iommu/iommufd/io_pagetable.o drivers/iommu/iommufd/pages.o \
  drivers/iommu/iommufd/device.o drivers/iommu/iommufd/ioas.o \
  drivers/iommu/iommufd/vfio_compat.o drivers/vfio/group.o \
  drivers/vfio/container.o drivers/vfio/vfio_iommu_type1.o \
  drivers/iommu/riscv/iommu.o -j 16
```

Boot host:

```sh
env QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh
env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE=edu QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh
env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE=edu HOST_COVE_IO_TEST=vfio-msi \
  QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh
```

Guest probes:

```sh
COVE_IO_TEST_PROBE=1 ./run-guest-os.sh
COVE_IO_TEST_PROBE=1 COVE_IO_TEST_SKIP=virtio ./run-guest-os.sh
COVE_IO_TEST_PROBE=1 COVE_IO_TEST_SKIP=virtio-dma ./run-guest-os.sh
COVE_IO_TEST_PROBE=1 COVE_IO_TEST_SKIP=virtio-irq ./run-guest-os.sh
GUEST_BALLOON=1 COVE_IO_TEST_PROBE=1 ./run-guest-os.sh
```

VFIO PCI prototype inside the booted host:

```sh
VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_UNSAFE_INTERRUPTS=0 ./vfio-bind-pci.sh
GUEST_VFIO_PCI=0000:00:02.0 COVE_IO_TEST_PROBE=1 ./run-guest-os.sh
COVE_IO_TEST_SKIP=vfio-mmio GUEST_VFIO_PCI=0000:00:02.0 COVE_IO_TEST_PROBE=1 ./run-guest-os.sh
COVE_IO_TEST_SKIP=vfio-dma GUEST_VFIO_PCI=0000:00:02.0 COVE_IO_TEST_PROBE=1 ./run-guest-os.sh
COVE_IO_TEST_SKIP=vfio-irq GUEST_VFIO_PCI=0000:00:02.0 COVE_IO_TEST_PROBE=1 ./run-guest-os.sh
COVE_IO_TEST_BAD_SWIOTLB_DEVICE=vfio GUEST_VFIO_PCI=0000:00:02.0 ./run-guest-os.sh
COVE_IO_TEST_BAD_SWIOTLB_IOMMU_GROUP=vfio GUEST_VFIO_PCI=0000:00:02.0 ./run-guest-os.sh
COVE_IO_TEST_BAD_IOMMU_GROUP=vfio GUEST_VFIO_PCI=0000:00:02.0 ./run-guest-os.sh
COVE_IO_TEST_BAD_SWIOTLB_RID=vfio GUEST_VFIO_PCI=0000:00:02.0 ./run-guest-os.sh
COVE_IO_TEST_BAD_TDI_RID=vfio GUEST_VFIO_PCI=0000:00:02.0 ./run-guest-os.sh
```

Use the BDF printed by `vfio-bind-pci.sh` if it differs from the example.
`COVE_IO_TEST_SKIP=vfio-iommu-map` is available for IOMMU fault injection: it
keeps the COVE-IO TDI state and VFIO device identity registered while using a
private lazy-map flag. The default legacy backend uses
`VFIO_DMA_MAP_FLAG_COVE_IO_LAZY`, which creates the VFIO type1 DMA
ownership/accounting entry for the restricted DMA window but does not install
IOMMU PTEs up front. On an authorized fault, VFIO type1 pins and maps the
missing 4K page. kvmtool can also select the iommufd VFIO compatibility backend
with `COVE_IO_TEST_VFIO_BACKEND=iommufd`; that path opens `/dev/iommu`,
allocates an IOAS, sets it as the VFIO compatibility IOAS, attaches VFIO groups
to the iommufd, and maps/unmaps DMA through `IOMMU_IOAS_MAP/UNMAP` with
`IOMMU_IOAS_MAP_COVE_IO_LAZY`. The iommufd lazy path is intentionally
per-device: kvmtool allocates a private IOAS/HWPT for each COVE-IO VFIO
device, switches the VFIO compatibility IOAS before opening each device fd,
and rejects same-IOMMU-group reuse because the group compatibility path cannot
represent two private HWPTs in one VFIO group. The kernel still rejects
multi-domain or shared-HWPT recovery paths if they are reached.
`COVE_IO_TEST_PAUSE_BEFORE_RUN=<seconds>` pauses kvmtool
after VM, VFIO, SWIOTLB, and TDI initialization but before starting vCPU
threads; combine it with `COVE_IO_TEST_SKIP=vcpu-run` to keep the VM alive long
enough for host IOMMU fault injection and then exit without running guest code.
`scripts/cove-io-vfio-lazy-fault-test.sh` automates the VFIO lazy fault
recovery check inside the booted host: it binds the VFIO test PCI device,
starts a paused lazy-map VM, triggers repeated and adjacent-page RISC-V IOMMU
debug translations, and fails if recovery does not install mappings or if
kernel warnings are observed. It defaults to the legacy VFIO type1 backend; run
it with `COVE_IO_TEST_VFIO_BACKEND=iommufd` or `VFIO_BACKEND=iommufd` to cover
the iommufd VFIO compatibility map/unmap path. In iommufd mode the script also
starts a duplicate-BDF negative VM and requires kvmtool to reject same-group
COVE-IO iommufd compat reuse before creating a second per-device IOAS
attachment. After kvmtool exits, it also repeats the first translation and
fails if the same successful translation remains installed, covering the VM
teardown/VFIO detach unmap path.
`scripts/cove-io-vfio-multi-test.sh` automates the two-device iommufd policy
check: it binds two VFIO-capable `edu` PCI devices in different IOMMU groups,
starts a paused CVM with both devices assigned, requires kvmtool to allocate
two COVE-IO iommufd IOAS objects and map the restricted DMA window into both,
then triggers RISC-V IOMMU debug translations for both PCI requester IDs. The
test fails unless both RIDs recover their own lazy mapping and both mappings
disappear after teardown.
`scripts/cove-io-vfio-msi-test.sh` automates the observable VFIO MSI/MSI-X IRQ
authorization check inside the booted host: it binds the VFIO test PCI device,
first runs the deterministic VFIO COVE-IO probe path and requires
`vfio-irq-target-iid=allow`, `vfio-irq-target-device=allow`,
`vfio-irq-target-wrong-device=deny`, `vfio-irq-target-wrong-iid=deny`,
`vfio-irq-target-wrong-vcpu=deny`, and `vfio-irq-target-iid-unbound=deny`.
It then starts a second CVM with the VFIO PCI device attached and passes
`GUEST_COVE_IO_AUTORUN=edu-msi` to the nested guest. The guest autorun hook
finds the QEMU `edu` PCI device, writes an IMSIC MSI address and non-zero MSI
data into the device's MSI capability, and enables MSI. The host-side script
fails if any `allow_unsafe_interrupts` path is enabled, requires that the
RISC-V IOMMU COVE-IO MSI remap initialized, requires VFIO/iommufd to consume
that remap path instead of generic isolated MSI, fails if kvmtool hides
MSI/MSI-X capabilities, requires a `COVE-IO dynamic MSI/MSI-X IRQ bind` log,
and requires that the dynamic bind log records a non-zero `iid=`.
With `COVE_IO_TEST_MSI_MODE=retarget`, or host autorun
`HOST_COVE_IO_TEST=vfio-msi-retarget`, the same script runs the deterministic
probe with two vCPUs, verifies a vCPU0 to vCPU1 IRQ retarget, requires the old
vCPU target to be denied, requires the new vCPU target to be allowed, and
accepts either an installed/refreshed vCPU1 MRIF PTE or a pending vCPU1 MRIF
binding that explicitly blocks MSI until the vCPU1 IMSIC VS-file exists.
`scripts/cove-io-vfio-pri-test.sh` automates the observable VFIO PRI path:
it enables the COVE-IO RISC-V IOMMU PRI parameter, binds the VFIO test PCI
device without `allow_unsafe_interrupts`, starts a CVM with lazy VFIO IOMMU
mapping, and passes `GUEST_COVE_IO_AUTORUN=edu-pri` to the nested guest. The
guest programs the QEMU `edu` PCIe ATS/PRI test registers, the QEMU RISC-V
IOMMU enqueues a page request, Linux handles the PRQ entry, COVE-IO authorizes
the IOVA, VFIO installs the lazy mapping, and Linux sends a PRGR success
response back to the device.
For unattended host-side validation, `boot-host-os.sh` accepts
`HOST_COVE_IO_TEST=vfio-msi`, `HOST_COVE_IO_TEST=vfio-msi-retarget`,
`HOST_COVE_IO_TEST=vfio-msi-retarget-stress`,
`HOST_COVE_IO_TEST=vfio-lazy`, `HOST_COVE_IO_TEST=vfio-multi`,
`HOST_COVE_IO_TEST=vfio-pri`, `HOST_COVE_IO_TEST=vfio-pri-deny`,
`HOST_COVE_IO_TEST=vfio-pri-overflow`, `HOST_COVE_IO_TEST=vfio-pri-stress`,
`HOST_COVE_IO_TEST=vfio-pri-pasid`, `HOST_COVE_IO_TEST=vfio-pri-cancel`,
`HOST_COVE_IO_TEST=vfio-pri-stop`, or
`HOST_COVE_IO_TEST=vfio-pri-extended`. The host initramfs autorun hook reads
this from the kernel command line, runs the matching
`/scripts/cove-io-vfio-*.sh` test inside the booted RISC-V host guest, prints
the test log to the serial console, and powers off by default. Set
`HOST_COVE_IO_TEST_POWER_OFF=0` to keep the host guest running after the test.
The `COVE_IO_TEST_BAD_SWIOTLB_DEVICE=vfio` command is expected to fail with a
non-zero exit after OpenSBI rejects the runtime DMA share request.
The `COVE_IO_TEST_BAD_SWIOTLB_IOMMU_GROUP=vfio` command is expected to fail
while registering the VFIO SWIOTLB sub-pool, with Linux/KVM rejecting the
mismatched PCI RID and IOMMU group before the runtime DMA path is reached.
The `COVE_IO_TEST_BAD_IOMMU_GROUP=vfio` command is expected to fail earlier,
while registering the VFIO PCI DMA TDI, with Linux/KVM rejecting the mismatched
PCI RID and IOMMU group.
The `COVE_IO_TEST_BAD_SWIOTLB_RID=vfio` command is expected to fail while
registering the VFIO SWIOTLB sub-pool, with Linux/KVM rejecting the PCI RID
because it is not present in the kernel IOMMU fwspec ids for the PCI device.
The `COVE_IO_TEST_BAD_TDI_RID=vfio` command is expected to fail while
registering the VFIO PCI DMA TDI for the same reason.

Expected probe matrix:

| Command | default-mmio | pci-mmio | virtio-mmio | virtio-dma | virtio-dma-wrong-device | virtio-dma-unknown-type | virtio-irq |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `COVE_IO_TEST_PROBE=1` | allow | allow | allow | allow | deny | deny | allow |
| `COVE_IO_TEST_SKIP=virtio` | allow | allow | deny | deny | deny | deny | deny |
| `COVE_IO_TEST_SKIP=virtio-dma` | allow | allow | allow | deny | deny | deny | allow |
| `COVE_IO_TEST_SKIP=virtio-irq` | allow | allow | allow | allow | deny | deny | deny |
| `GUEST_BALLOON=1 COVE_IO_TEST_PROBE=1` | allow | allow | allow for both virtio-mmio devices | allow for both virtio-mmio devices | deny for both virtio-mmio devices | deny for both virtio-mmio devices | allow for both virtio-mmio devices |

With `GUEST_VFIO_PCI=<bdf> COVE_IO_TEST_PROBE=1`, the additional expected VFIO
probe matrix is:

| Command | vfio-mmio | vfio-dma | vfio-dma-wrong-device | vfio-dma-wrong-rid | vfio-dma-unknown-type | vfio-irq |
| --- | --- | --- | --- | --- | --- | --- |
| `GUEST_VFIO_PCI=<bdf> COVE_IO_TEST_PROBE=1` | allow | allow | deny | deny | deny | allow |
| `COVE_IO_TEST_SKIP=vfio-mmio` | deny | allow | deny | deny | deny | allow |
| `COVE_IO_TEST_SKIP=vfio-dma` | allow | deny | deny | deny | deny | allow |
| `COVE_IO_TEST_SKIP=vfio-irq` | allow | allow | deny | deny | deny | deny |

The positive VFIO IRQ probe also performs a deterministic multi-range IRQ
authorization check without depending on a real MSI/MSI-X-capable guest driver:
kvmtool temporarily binds two sparse high-numbered IRQs into the already
started VFIO TDI, verifies that both bound IRQs are allowed while the
intermediate hole is denied, unbinds both temporary ranges, and verifies that
the endpoints are denied again. It also binds a deterministic target-vCPU/IID
tuple, verifies that the exact tuple and the exact VFIO PCI `device_id` are
allowed, and verifies that the same GSI with the wrong VFIO PCI requester ID,
wrong target vCPU, or wrong IMSIC interrupt ID is denied.

Verified on 2026-07-01 with host QEMU `riscv-iommu-pci` plus `edu` PCI device:

- Host detected `0000:00:01.0` as `riscv-iommu-pci`.
- Host detected `0000:00:02.0 [1234:11e8]` as the `edu` test device in IOMMU
  group 1.
- `VFIO_ALLOW_UNSAFE_INTERRUPTS=1 ./vfio-bind-pci.sh` bound the device to
  `vfio-pci` and created `/dev/vfio/1`.
- Positive VFIO run printed `Using IOMMU type 3 for VFIO container`,
  `VFIO CVM DMA window: iova=0x82d00000 size=0x100000`, and
  `device_id=0x200000000000010 segment=0x0 rid=0x10 iommu_group=1` for
  `0000:00:02.0`. The allowed VFIO probes returned `allow`.
- Negative VFIO runs returned `vfio-mmio=deny`, `vfio-dma=deny`, and
  `vfio-irq=deny` respectively.

Additional device-id probe coverage verified on 2026-07-01:

- Normal virtio DMA lookup returned `virtio-dma=allow`.
- The same virtio DMA GPA queried with a wrong device id returned
  `virtio-dma-wrong-device=deny`.
- The same virtio DMA GPA queried with an unknown device-id type returned
  `virtio-dma-unknown-type=deny`.
- Normal VFIO DMA lookup returned `vfio-dma=allow`.
- The same VFIO DMA GPA queried with a wrong device id returned
  `vfio-dma-wrong-device=deny`.
- The same VFIO DMA GPA queried with the same PCI segment but a different
  requester ID returned `vfio-dma-wrong-rid=deny`.
- The same VFIO DMA GPA queried with an unknown device-id type returned
  `vfio-dma-unknown-type=deny`.
- Runtime bad-SWIOTLB-device coverage registered the VFIO SWIOTLB sub-pool with
  `KVM_COVE_IO_DEVICE_INVALID` while leaving the VFIO TDI correct. OpenSBI
  rejected the runtime DMA share at `gpa=0x82d00000`, Linux reported
  `kvm: CVM run returned -2`, and kvmtool exited with status 1 instead of
  timing out or repeatedly faulting.
- Bad-SWIOTLB-IOMMU-group coverage incremented the VFIO SWIOTLB sub-pool's
  IOMMU group while leaving its PCI-RID `device_id` correct. Linux/KVM rejected
  the `KVM_SET_SWIOTLB` registration with
  `COVE-IO SWIOTLB PCI RID/iommu_group mismatch device_id=0x200000000000010 group=2`,
  kvmtool reported `KVM_SET_SWIOTLB ioctl: Operation not permitted`, and the
  command exited with status 1.
- Bad-IOMMU-group coverage incremented the VFIO IOMMU group id before the
  VFIO PCI TDI `DMA_MAP`. Linux/KVM rejected the registration before the
  OpenSBI TDI call with
  `COVE-IO PCI RID/iommu_group mismatch device_id=0x200000000000010 group=2`,
  kvmtool reported `KVM_COVE_IO_TDI_OP ioctl: Operation not permitted`, and
  the command exited with status 1.

Additional PCI-RID/fwspec coverage verified on 2026-07-02 with host QEMU
`riscv-iommu-pci` plus `edu` PCI device:

- Positive VFIO run still completed with exit status 0 and printed
  `device_id=0x200000000000010 segment=0x0 rid=0x10 iommu_group=1` for
  `0000:00:02.0`. The VFIO probe matrix still returned `vfio-mmio=allow`,
  `vfio-dma=allow`, `vfio-dma-wrong-rid=deny`, and `vfio-irq=allow`.
- Bad-SWIOTLB-RID coverage incremented the VFIO SWIOTLB sub-pool's PCI
  requester ID while leaving its IOMMU group id correct. Linux/KVM rejected the
  `KVM_SET_SWIOTLB` registration with
  `COVE-IO SWIOTLB PCI RID/iommu_group mismatch device_id=0x200000000000011 group=1`,
  kvmtool reported `KVM_SET_SWIOTLB ioctl: Operation not permitted`, and the
  command exited with status 1.
- Bad-TDI-RID coverage incremented the VFIO PCI requester ID before the VFIO
  PCI TDI `DMA_MAP` while leaving its IOMMU group id correct. Linux/KVM
  rejected the registration before the OpenSBI TDI call with
  `COVE-IO PCI RID/iommu_group mismatch device_id=0x200000000000011 group=1`,
  kvmtool reported `KVM_COVE_IO_TDI_OP ioctl: Operation not permitted`, and
  the command exited with status 1.

RISC-V IOMMU fault recovery prototype coverage verified on 2026-07-02 with
host QEMU `riscv-iommu-pci` plus `edu,dma_mask=0xffffffffffffffff`:

- The QEMU host exposed the RISC-V IOMMU as `0000:00:01.0` and the `edu` VFIO
  test device as `0000:00:02.0`. The VFIO device used COVE-IO
  `device_id=0x200000000000010`, PCI segment 0, requester ID `0x10`, IOMMU
  group 1, and VFIO DMA window `0x82d00000+0x100000`.
- With `COVE_IO_TEST_SKIP=vfio-iommu-map,vcpu-run` and
  `COVE_IO_TEST_PAUSE_BEFORE_RUN=90`, the VFIO TDI still included
  `dma=0x82d00000+0x100000`, and kvmtool registered the VFIO type1 DMA window
  with the private lazy flag. A RISC-V IOMMU debug translation request for
  DID/RID `0x10` and IOVA `0x82d00000` first produced a fault response
  `0x0000000000003C01` and this kernel log:
  `Fault 15 devid: 0x10 iotval: 82d00000 iotval2: 0 cove_io=allow-vfio-map phys=0x0000000089184000 size=0x1000 rc=0`.
  Repeating the same translation request did not emit a second fault and
  returned translated response `0x0000000022461000`, proving that the
  authorized fault installed a usable VFIO-managed IOMMU mapping.
- With `COVE_IO_TEST_VFIO_BACKEND=iommufd`,
  `COVE_IO_TEST_SKIP=vfio-iommu-map,vcpu-run`, and
  `COVE_IO_TEST_PAUSE_BEFORE_RUN=90`, kvmtool opened `/dev/iommu`, allocated
  VFIO compatibility IOAS 1, attached VFIO group 1 through iommufd, and mapped
  the restricted DMA window through `IOMMU_IOAS_MAP` with
  `IOMMU_IOAS_MAP_COVE_IO_LAZY`. Because the QEMU RISC-V platform does not
  provide isolated MSI/interrupt-remapping support, the test requires
  `/sys/module/iommufd/parameters/allow_unsafe_interrupts=Y`, matching the
  existing legacy `vfio_iommu_type1.allow_unsafe_interrupts` test override.
  Repeated debug translation requests for IOVA `0x82d00000` and adjacent IOVA
  `0x82d01000` first faulted and then succeeded, with kernel logs:
  `Fault 15 devid: 0x10 iotval: 82d00000 iotval2: 0 cove_io=allow-vfio-map phys=0x00000000a390a000 size=0x1000 rc=0`
  and
  `Fault 15 devid: 0x10 iotval: 82d01000 iotval2: 0 cove_io=allow-vfio-map phys=0x00000000a3919000 size=0x1000 rc=0`.
  The automated script reported:
  `PASS: VFIO lazy fault recovery repeated, adjacent-page, and teardown checks passed (iommufd)`.
  After teardown, the same IOVA translated to the platform default/identity
  domain response rather than the CVM physical page response, so the VFIO/CVM
  mapping was not left installed.
- On 2026-07-07 the kvmtool iommufd compat path was changed from one shared
  compat IOAS to per-device IOAS allocation for COVE-IO. kvmtool now switches
  `IOMMU_VFIO_IOAS_SET` to the VFIO device's private IOAS before
  `VFIO_GROUP_GET_DEVICE_FD`, maps the CVM restricted DMA window into that
  private IOAS with `IOMMU_IOAS_MAP_COVE_IO_LAZY`, and unmaps/destroys it at
  device teardown. The fail-closed policy is now scoped to VFIO IOMMU groups:
  a duplicate-BDF/same-group COVE-IO iommufd compat VM must fail with
  `COVE-IO iommufd compat requires one VFIO IOMMU group per device`.
  The rerun completed successfully with:
  `PASS: VFIO lazy fault recovery repeated, adjacent-page, and teardown checks passed (iommufd)`.
  The observed responses were
  `first=0x0000000000003C01 second=0x0000000021774800 third=0x0000000000003C01 fourth=0x0000000022582C00 post_exit=0x0000000020B40000`,
  and the host autorun reported `COVE-IO host autorun: PASS vfio-lazy`.
- With `COVE_IO_TEST_SKIP=vfio-dma,vfio-iommu-map,vcpu-run` and
  `COVE_IO_TEST_PAUSE_BEFORE_RUN=90`, the VFIO TDI had no DMA range
  (`dma=0x82d00000+0x0`) while the SWIOTLB sub-pool still carried the matching
  PCI RID. Two identical debug translation requests both produced fault
  response `0x0000000000003C01` and logged
  `Fault 15 devid: 0x10 iotval: 82d00000 iotval2: 0 cove_io=deny-observe`.
  Denied faults therefore do not install mappings.
- With an authorized VFIO TDI, DID/RID `0x10` and out-of-pool IOVA
  `0x82e00000` fell back to the normal RISC-V IOMMU fault log without a
  `cove_io=` suffix. A wrong DID/RID request also fell back to the normal fault
  path.
- Fault injection used the RISC-V IOMMU debug translation request registers:
  `TR_REQ_IOVA = IOMMU_BAR + 0x258`, `TR_REQ_CTL = IOMMU_BAR + 0x260`, and
  `TR_RESPONSE = IOMMU_BAR + 0x268`. For DID/RID `0x10`, the control word was
  `0x100000000001`. Direct `edu` BAR-driven DMA did not emit a Linux RISC-V
  IOMMU fault log in this QEMU setup, so the debug translation request path was
  used to exercise QEMU's RISC-V IOMMU fault queue deterministically.
- A normal non-lazy VFIO probe run after this change completed with
  `PROBE_RC=0`; the VFIO probe matrix still returned `vfio-mmio=allow`,
  `vfio-dma=allow`, `vfio-dma-wrong-device=deny`, `vfio-dma-wrong-rid=deny`,
  `vfio-dma-unknown-type=deny`, and `vfio-irq=allow`.

Multi-device iommufd COVE-IO lazy-fault coverage verified on 2026-07-07 with
host QEMU `riscv-iommu-pci` plus two
`edu,dma_mask=0xffffffffffffffff` PCI devices:

- Ran
  `env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICES='edu,dma_mask=0xffffffffffffffff edu,dma_mask=0xffffffffffffffff' HOST_COVE_IO_TEST=vfio-multi HOST_COVE_IO_TEST_WAIT_SECS=90 HOST_COVE_IO_TEST_POWER_OFF=1 QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh`.
- The host exposed `0000:00:02.0` in IOMMU group 1 with requester ID `0x10`
  and `0000:00:03.0` in IOMMU group 2 with requester ID `0x18`.
- kvmtool used the iommufd VFIO compatibility backend, allocated two private
  COVE-IO IOAS objects, selected the matching IOAS before opening each VFIO
  device fd, and mapped the restricted VFIO DMA window into both IOAS objects
  with `IOMMU_IOAS_MAP_COVE_IO_LAZY`.
- The first debug translation for each requester ID faulted, and the repeated
  translation for each requester ID succeeded after COVE-IO authorization and
  iommufd lazy mapping. The kernel log contained
  `cove_io=allow-vfio-map` for both `devid: 0x10` and `devid: 0x18`.
- After kvmtool teardown, translations no longer returned the successful CVM
  mapping responses, covering per-device IOAS unmap and TDI cleanup.
- The automated host autorun completed with
  `PASS: VFIO multi-device per-IOAS lazy fault recovery and teardown checks passed (iommufd)`
  and `COVE-IO host autorun: PASS vfio-multi`. The observed summary was
  `dev1=0000:00:02.0 rid=0x10 group=1 first=0x0000000000003C01 second=0x0000000022595000 post=0x0000000020B40000`
  and
  `dev2=0000:00:03.0 rid=0x18 group=2 first=0x0000000000003C01 second=0x0000000022595000 post=0x0000000020B40000`.

Multi-range VFIO IRQ authorization coverage verified on 2026-07-03 with host
QEMU `riscv-iommu-pci` plus `edu,dma_mask=0xffffffffffffffff`:

- `VFIO_ALLOW_UNSAFE_INTERRUPTS=1 ./vfio-bind-pci.sh 0000:00:02.0` bound the
  `edu` test device to `vfio-pci` in IOMMU group 1.
- `GUEST_VFIO_PCI=0000:00:02.0 COVE_IO_TEST_PROBE=1 ./run-guest-os.sh`
  completed with `KVM session ended normally`.
- The VFIO probe matrix returned `vfio-mmio=allow`, `vfio-dma=allow`,
  `vfio-dma-wrong-device=deny`, `vfio-dma-wrong-rid=deny`,
  `vfio-dma-unknown-type=deny`, and `vfio-irq=allow`.
- The deterministic sparse IRQ range probe returned
  `vfio-irq-sparse-first=allow`,
  `vfio-irq-sparse-first-wrong-vcpu=deny`, `vfio-irq-sparse-hole=deny`,
  `vfio-irq-sparse-second=allow`, `vfio-irq-sparse-first-unbound=deny`, and
  `vfio-irq-sparse-second-unbound=deny`, proving that sparse IRQ authorization
  does not collapse into an over-broad min/max range, that IRQ authorization is
  target-vCPU aware, and that range-specific unbind removes the temporary
  authorizations.
- A follow-up code-only update added IMSIC interrupt-ID matching to the same IRQ
  authorization path. The deterministic VFIO IRQ probe now also expects
  `vfio-irq-target-iid=allow`, `vfio-irq-target-wrong-iid=deny`,
  `vfio-irq-target-wrong-vcpu=deny`, and `vfio-irq-target-iid-unbound=deny`;
  `scripts/cove-io-vfio-msi-test.sh` checks those probe logs before its real
  MSI/MSI-X run.

VFIO MSI/MSI-X and IMSIC-IID runtime coverage verified on 2026-07-06 with host
QEMU `riscv-iommu-pci` plus `edu` PCI device:

- Rebuilt the host Linux Image, kvmtool `lkvm-static`, and host/nested guest
  initramfs images so the runtime used the current COVE-IO KVM/VFIO/IOMMU code.
- Ran
  `env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE=edu HOST_COVE_IO_TEST=vfio-msi HOST_COVE_IO_TEST_WAIT_SECS=45 HOST_COVE_IO_TEST_POWER_OFF=1 QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh`.
- Host Linux detected `0000:00:01.0 [1b36:0014]` as the QEMU RISC-V IOMMU and
  `0000:00:02.0 [1234:11e8]` as the `edu` VFIO test device in IOMMU group 1.
- The deterministic probe stage passed the full matrix, including
  `vfio-dma=allow`, `vfio-dma-wrong-rid=deny`,
  `vfio-irq-target-iid=allow`, `vfio-irq-target-wrong-iid=deny`,
  `vfio-irq-target-wrong-vcpu=deny`, and
  `vfio-irq-target-iid-unbound=deny`.
- The second CVM used nested guest autorun `edu-msi` to enable MSI for the
  assigned `edu` device. The host-side test observed
  `COVE-IO dynamic MSI/MSI-X IRQ bind` with a non-zero `iid=`, and the full
  autorun completed with
  `PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind and IID probe observed (legacy)`.

COVE-IO RISC-V IOMMU MSI remap coverage verified on 2026-07-06 with host QEMU
`riscv-iommu-pci` plus `edu` PCI device:

- Rebuilt the full host Linux Image and regenerated the host/nested initramfs.
  The host Linux build completed with no compiler errors for the touched
  IOMMU/VFIO/iommufd objects.
- `vfio-bind-pci.sh` now writes the COVE-IO isolated MSI parameter from
  `/sys/module/*/parameters/cove_io_isolated_msi`, which covers the current
  built-in object name `/sys/module/iommu/parameters/cove_io_isolated_msi`.
  With `VFIO_ALLOW_UNSAFE_INTERRUPTS=0`, it also fails if existing
  `vfio_iommu_type1.allow_unsafe_interrupts` or `iommufd.allow_unsafe_interrupts`
  state is already enabled.
- The host autorun command
  `env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE=edu HOST_COVE_IO_TEST=vfio-msi HOST_COVE_IO_TEST_WAIT_SECS=45 HOST_COVE_IO_TEST_POWER_OFF=1 QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh`
  completed successfully without enabling `allow_unsafe_interrupts`.
- The kernel log showed
  `COVE-IO experimental MSI basic-mode remap enabled: entries=1 table_size=4096 pattern=0x28000 mask=0x0`
  for the QEMU single-target IMSIC topology, followed by
  `using experimental COVE-IO RISC-V IOMMU MSI remap instead of generic isolated MSI`
  from VFIO type1 attach.
- The same run completed with
  `PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind and IID probe observed (legacy)`
  and `COVE-IO host autorun: PASS vfio-msi`.

COVE-IO RISC-V IOMMU MRIF posted-interrupt prototype verified on 2026-07-07
with host QEMU `riscv-iommu-pci` plus `edu` PCI device:

- Built the touched Linux KVM/AIA and RISC-V IOMMU objects, rebuilt the host
  Linux `Image`, rebuilt `kvmtool/lkvm-static`, and refreshed the host/nested
  initramfs with the updated VFIO scripts.
- Ran
  `env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE=edu HOST_COVE_IO_TEST=vfio-msi HOST_COVE_IO_TEST_WAIT_SECS=120 HOST_COVE_IO_TEST_POWER_OFF=1 QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh`.
- The host command line enabled `iommu.cove_io_isolated_msi=1` and
  `iommu.cove_io_mrif=1`.
- VFIO attach still used the COVE-IO isolated MSI path instead of
  `allow_unsafe_interrupts`.
- The nested guest enabled MSI/MSI-X for the VFIO `edu` device. kvmtool passed
  the VFIO PCI COVE-IO `device_id` through the dynamic IRQ bind, KVM accepted
  the OpenSBI target-vCPU/IID authorization, and Linux installed a per-device
  MRIF PTE:
  `COVE-IO experimental MRIF remap installed: device_id=0x200000000000010 vcpu=0 iid=113 index=0 mrif=0x000000008b6e2000 notice=0x0000000028001000`.
- The automated host autorun completed with
  `PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind and IID probe observed (legacy)`
  and `COVE-IO host autorun: PASS vfio-msi`.

COVE-IO device-scoped VFIO MSI/MRIF interrupt confinement verified on
2026-07-07 with host QEMU `riscv-iommu-pci` plus `edu` PCI device:

- Linux targeted objects
  `arch/riscv/kvm/vm.o`, `arch/riscv/kvm/vcpu.o`,
  `arch/riscv/kvm/aia_imsic.o`, and `drivers/iommu/riscv/iommu.o` built
  successfully.
- Rebuilt the host Linux `Image`, OpenSBI `PLATFORM=generic`, and
  `kvmtool/lkvm-static`, then refreshed the host/nested initramfs.
- Ran
  `env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE=edu HOST_COVE_IO_TEST=vfio-msi HOST_COVE_IO_TEST_WAIT_SECS=120 HOST_COVE_IO_TEST_POWER_OFF=1 QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh`.
- The deterministic VFIO probe path now requires the exact VFIO PCI
  `device_id` and rejects the same GSI/vCPU/IID tuple when queried with the
  same PCI segment but a different requester ID.
- The dynamic VFIO MSI/MRIF run installed a per-device MRIF PTE:
  `COVE-IO experimental MRIF remap installed: device_id=0x200000000000010 vcpu=0 iid=113 index=0 mrif=0x000000008a894000 notice=0x0000000028001000`.
- The automated host autorun completed with
  `PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind, device, and IID probes observed (legacy)`
  and `COVE-IO host autorun: PASS vfio-msi`.

COVE-IO MRIF retarget and pending-blocked coverage verified on 2026-07-07 with
host QEMU `riscv-iommu-pci` plus `edu,dma_mask=0xffffffff`:

- Rebuilt the host Linux `Image` after the KVM TDI ioctl and RISC-V IOMMU MRIF
  updates, refreshed the host/nested initramfs, and ran
  `env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE='edu,dma_mask=0xffffffff' HOST_COVE_IO_TEST=vfio-msi-retarget HOST_COVE_IO_TEST_WAIT_SECS=120 ./boot-host-os.sh`.
- The deterministic two-vCPU probe authorized the initial vCPU0 target, denied
  the old vCPU0 target after retarget, authorized the new vCPU1 target, and
  denied the target again after unbind:
  `vfio-irq-mrif-retarget-vcpu0=allow`,
  `vfio-irq-mrif-retarget-old-vcpu=deny`,
  `vfio-irq-mrif-retarget-vcpu1=allow`, and
  `vfio-irq-mrif-retarget-unbound=deny`.
- Because the probe intentionally stops before entering the vCPU run-loop,
  vCPU1 has no IMSIC VS-file yet. The RISC-V IOMMU path therefore recorded the
  new vCPU1 MRIF binding and blocked the device's MSI table instead of leaving
  the old vCPU0 MRIF PTE active:
  `COVE-IO MRIF remap retarget: device_id=0x200000000000010 old_vcpu=0 new_vcpu=1 iid=6`
  followed by
  `COVE-IO experimental MRIF remap pending: device_id=0x200000000000010 vcpu=1 iid=6; MSI blocked until IMSIC VS-file is available`.
- The second single-vCPU MSI run still installed a concrete per-device MRIF PTE
  for the running CVM:
  `COVE-IO experimental MRIF remap installed: device_id=0x200000000000010 vcpu=0 iid=113 index=0 mrif=0x000000008a62b000 notice=0x0000000028001000`.
- The automated host autorun completed with
  `PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind mode=retarget, device, and IID probes observed (legacy)`
  and `COVE-IO host autorun: PASS vfio-msi-retarget`.

COVE-IO MRIF repeated retarget pressure coverage verified on 2026-07-07 with
host QEMU `riscv-iommu-pci` plus `edu,dma_mask=0xffffffff`:

- Added `COVE_IO_TEST_MRIF_RETARGET_LOOPS` to kvmtool's deterministic VFIO IRQ
  probe. The probe now repeatedly rebinds the same VFIO PCI GSI across vCPUs,
  checks that the previous vCPU/IID tuple is denied after each replacement,
  checks that the new vCPU/IID tuple is allowed, and checks that the final
  target is denied after unbind.
- Added host autorun support for `HOST_COVE_IO_TEST=vfio-msi-retarget-stress`
  and `HOST_COVE_IO_MRIF_RETARGET_LOOPS=<n>`.
- Rebuilt `kvmtool/lkvm-static`, refreshed the host/nested initramfs, and ran
  `env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE='edu,dma_mask=0xffffffff' HOST_COVE_IO_TEST=vfio-msi-retarget-stress HOST_COVE_IO_MRIF_RETARGET_LOOPS=8 HOST_COVE_IO_TEST_WAIT_SECS=180 ./boot-host-os.sh`.
- The probe created a four-vCPU CVM and performed eight MRIF retarget rounds.
  Host Linux logged the retarget chain across vCPU0, vCPU1, vCPU2, and vCPU3,
  and every pending target explicitly blocked MSI until that vCPU's IMSIC
  VS-file was available.
- The follow-up single-vCPU MSI run still installed a concrete per-device MRIF
  PTE for the authorized VFIO PCI device:
  `COVE-IO experimental MRIF remap installed: device_id=0x200000000000010 vcpu=0 iid=113 index=0 ...`.
- The automated host autorun completed with
  `PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind mode=retarget-stress, device, and IID probes observed (legacy)`
  and `COVE-IO host autorun: PASS vfio-msi-retarget-stress`.

VFIO PRI page-request recovery coverage verified on 2026-07-06 with host QEMU
`riscv-iommu-pci` plus PCIe `edu` test device:

- Ran
  `env HOST_RISCV_IOMMU=1 HOST_PCI_TEST_DEVICE=edu HOST_COVE_IO_TEST=vfio-pri HOST_COVE_IO_TEST_WAIT_SECS=300 HOST_COVE_IO_TEST_POWER_OFF=1 QEMU_EXTRA_ARGS=-no-reboot ./boot-host-os.sh`.
- Host Linux enabled both COVE-IO RISC-V IOMMU knobs from the command line:
  `iommu.cove_io_pri=1` and `iommu.cove_io_isolated_msi=1`.
- Host Linux detected the QEMU RISC-V IOMMU and `edu` device, initialized
  COVE-IO MSI basic-mode remap, enabled the RISC-V IOMMU PRQ, and enabled
  PCIe ATS/PRI page-request recovery for `0000:00:02.0`.
- VFIO attach used the COVE-IO isolated MSI path instead of generic isolated
  MSI or `allow_unsafe_interrupts`, then enabled PCIe PRI recovery for the
  VFIO-bound `edu` device.
- The nested guest `edu-pri` autorun triggered a PRI request for IOVA
  `0x82d00000`. QEMU logged
  `COVE-IO PRI enqueue: devid=0x10 ...` and
  `COVE-IO PRI request accepted: devid=0x10 ... iova=0x82d00000 prgi=7`.
- The host RISC-V IOMMU consumed one PRQ entry and logged
  `Page request devid: 0x10 pasid: 0x0 pv:0 prgi:7 iova:82d00000 perms:7 cove_io=pri-vfio-map ... rc=0 resp=0`.
  QEMU then observed `COVE-IO PRI PRGR: devid=0x10 ... prgi=7 resp=0`.
- The automated host autorun completed with
  `PASS: VFIO PRI page-request recovery and PRGR response observed (legacy)`
  and `COVE-IO host autorun: PASS vfio-pri`.

VFIO PRI deny, overflow, and PASID-observable coverage verified on 2026-07-07
with host QEMU `riscv-iommu-pci` plus `edu,dma_mask=0xffffffff`:

- `HOST_COVE_IO_TEST=vfio-pri-deny` passed after the guest issued a PRI request
  for an out-of-range IOVA. KVM matched the request by PCI requester ID first,
  then denied the IOVA through the OpenSBI DMA lookup, and Linux emitted a
  PRGR non-success response:
  `COVE-IO PRI PRGR response: ... resp=1`.
- `HOST_COVE_IO_TEST=vfio-pri-overflow` passed and reported
  `PASS: VFIO PRI page-request recovery mode=overflow`, covering the QEMU PRQ
  pressure/overflow test mode without installing unauthorized mappings.
- `HOST_COVE_IO_TEST=vfio-pri-extended` passed after the guest generated page
  requests with PASID-valid entries for PASID 1 and PASID 2. The host log
  showed both PASIDs in PRQ/PRGR handling, and the script required PRGR
  responses for both requests.
- These tests exercise observable PRQ consumption, PRGR success/failure
  response generation, and multi-PASID request logging. They do not claim full
  Linux PCIe PRI subsystem equivalence for process-directory isolation.

Additional VFIO PRI semantic coverage verified on 2026-07-07 with the same
host QEMU `riscv-iommu-pci` plus `edu,dma_mask=0xffffffff` setup:

- `HOST_COVE_IO_TEST=vfio-pri-stop` passed and required the nested guest to
  print `COVE-IO guest edu-pri-stop`, covering the stop-marker-like QEMU `edu`
  test path without requiring PRQ consumption.
- `HOST_COVE_IO_TEST=vfio-pri-cancel` passed after the nested guest generated
  cancellable PRI requests and printed `COVE-IO guest edu-pri-cancel`. The host
  consumed the queued PRQ entries and QEMU observed PRGR responses.
- `HOST_COVE_IO_TEST=vfio-pri-stress HOST_COVE_IO_PRI_COUNT=64` passed after
  QEMU enqueued 64 PRI requests and the host RISC-V IOMMU consumed all 64
  entries in one PRQ interrupt before sending successful PRGR responses.
- `HOST_COVE_IO_TEST=vfio-pri-pasid` passed with a PASID-valid request for
  PASID 1. QEMU logged the accepted PASID request, Linux logged the same PASID
  in PRQ handling, and QEMU observed the matching PASID in PRGR.
- `HOST_COVE_IO_TEST=vfio-pri-extended` passed in one run with a 16-request
  burst, PASID-valid requests for PASID 1 and PASID 2, a cancellable request,
  and the stop-marker-like path.

Lifecycle closure build verification on 2026-07-06:

- OpenSBI `make -C opensbi CROSS_COMPILE=riscv64-linux-gnu- -j $(nproc)
  PLATFORM=generic` completed successfully. The tree still emits pre-existing
  OpenSBI warnings unrelated to the COVE-IO lifecycle changes.
- kvmtool `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu-
  LIBFDT_DIR=/nfs/home/liyunxiao/xs-cvm/dtc/lib64/lp64d lkvm-static
  -j $(nproc)` completed successfully after the unified TDI teardown and
  IMSIC-target-aware VFIO MSI/MSI-X authorization changes.
- Linux targeted KVM build
  `make -C linux O=/nfs/home/liyunxiao/xs-cvm/build-riscv64 ARCH=riscv
  CROSS_COMPILE=riscv64-linux-gnu- arch/riscv/kvm/vcpu.o
  arch/riscv/kvm/vm.o arch/riscv/kvm/aia_device.o -j $(nproc)` completed
  successfully. The sandboxed build environment printed NIS/whoami warnings,
  but there were no compiler errors for the touched KVM objects.
- `git diff --check` was clean for the touched OpenSBI, Linux/KVM, and kvmtool
  paths.

## Remaining Review Risks

- The ABI is private to this tree and intentionally not upstream-stable.
- The implementation authorizes device access only; it does not provide device
  traffic confidentiality or peer authentication.
- The current VFIO MSI/MSI-X runtime no longer depends on
  `VFIO_ALLOW_UNSAFE_INTERRUPTS=1`. The supported interrupt-safety model in
  this tree has two paths. The default path is VFIO eventfd/KVM injection with
  RISC-V IOMMU MSI basic-mode target filtering plus kvmtool/OpenSBI/Linux KVM
  GSI, PCI-RID device-id, target-vCPU, and IMSIC-IID authorization. The opt-in
  `iommu.cove_io_mrif=1` path installs per-device MRIF PTEs for dynamically
  authorized VFIO PCI MSI/MSI-X vectors and has end-to-end QEMU coverage. For
  the COVE-IO functional target, interrupt delivery is now device-scoped:
  probes deny wrong PCI requester IDs, wrong target vCPUs, and wrong IMSIC
  IIDs, the deterministic retarget probe revokes the old vCPU target and
  blocks MSI while the new vCPU's VS-file is pending, the dynamic MSI/MRIF run
  installs a per-device MRIF PTE for the authorized RID/vCPU/IID, and the
  retarget-stress run repeatedly replaces the target across four vCPUs while
  denying stale vCPU/IID tuples. This is still not a generic upstream-grade
  interrupt-remapping ABI: explicit non-compat iommufd HWPT UAPI integration,
  long-running live-migration or CPU-hotplug stress, and standard KVM/IOMMU ABI
  design remain outside this prototype.
- Runtime DMA authorization is device-id scoped, and VFIO PCI device ids are now
  derived from the PCI segment and requester ID. KVM now validates both PCI-RID
  `DMA_MAP` registration and PCI-RID SWIOTLB sub-pool registration against the
  kernel PCI device's IOMMU group and IOMMU fwspec requester IDs. The RISC-V
  IOMMU fault queue path now uses the DID/RID and IOVA reported by the host
  IOMMU driver, asks OpenSBI/KVM for COVE-IO authorization, and then uses the
  VFIO container/type1 or iommufd fault recovery hook to pin and map authorized
  pages. The type1 path now updates its DMA rbtree-backed range, `vfio_pfn`
  page tracking, and locked-memory accounting. The iommufd path is implemented
  as a per-device IOAS/HWPT policy: kvmtool maps each COVE-IO VFIO device's
  DMA window into a private IOAS and rejects same-IOMMU-group reuse, while the
  kernel rejects multi-domain or shared-HWPT recovery if reached. It has
  object-level build coverage, single-device repeated/adjacent-page/teardown
  lazy-fault coverage, and two-device different-IOMMU-group end-to-end
  coverage through `HOST_COVE_IO_TEST=vfio-multi`. The non-PRI fault-queue
  lazy recovery path still installs the missing mapping for a later DMA or
  translation rather than replaying the original failed DMA transaction. The
  PCIe PRI path is separate and now sends a PRGR success/failure response after
  COVE-IO authorization and VFIO lazy mapping; it has end-to-end PRQ plus PRGR
  success coverage through `HOST_COVE_IO_TEST=vfio-pri`, deny coverage through
  `HOST_COVE_IO_TEST=vfio-pri-deny`, queue-pressure/overflow coverage through
  `HOST_COVE_IO_TEST=vfio-pri-overflow`, 64-request stress coverage through
  `HOST_COVE_IO_TEST=vfio-pri-stress`, PASID-visible request/response coverage
  through `HOST_COVE_IO_TEST=vfio-pri-pasid` and
  `HOST_COVE_IO_TEST=vfio-pri-extended`, cancellation coverage through
  `HOST_COVE_IO_TEST=vfio-pri-cancel`, and stop-marker-like coverage through
  `HOST_COVE_IO_TEST=vfio-pri-stop`. The remaining PRI boundary is semantic
  depth rather than basic functionality: this prototype does not implement full
  Linux process-directory PASID isolation, has not been run against real
  PASID-rich hardware devices, and does not yet include long-duration,
  high-concurrency PRI soak testing.
- Current runtime coverage is sequential host/QEMU coverage with single-CVM
  and two-VFIO-device runs. It does not yet include concurrent multi-CVM stress,
  hot-unplug stress, or long-running migration/retarget loops.
- VFIO MSI/MSI-X dynamic authorization now uses multi-range TDI IRQ
  authorization instead of a single min/max range, and RISC-V AIA MSI injection
  is checked against the resolved PCI-RID device-id, target IMSIC/vCPU, and
  IMSIC interrupt ID. The current OpenSBI monitor prototype keeps a fixed
  `COVE_IO_MAX_IRQ_RANGES_PER_TDI=256` limit; extremely sparse MSI/MSI-X
  allocations beyond that limit fail the bind instead of authorizing
  intermediate GSIs.
- The IOMMU fault queue validation currently uses QEMU's simulated
  `riscv-iommu-pci`, not a physical RISC-V IOMMU device.
