#include "kvm/kvm.h"
#include "kvm/cove-io.h"
#include "kvm/vfio.h"
#include "kvm/ioport.h"

#include <linux/iommufd.h>
#include <linux/list.h>
#include <libgen.h>

#define VFIO_DEV_DIR		"/dev/vfio"
#define VFIO_DEV_NODE		VFIO_DEV_DIR "/vfio"
#define IOMMUFD_DEV_NODE	"/dev/iommu"
#define IOMMU_GROUP_DIR		"/sys/kernel/iommu_groups"
#define COVE_IO_VFIO_BACKEND_ENV "COVE_IO_TEST_VFIO_BACKEND"

enum vfio_backend {
	VFIO_BACKEND_LEGACY,
	VFIO_BACKEND_IOMMUFD_COMPAT,
};

static int vfio_container = -1;
static enum vfio_backend vfio_backend;
static int vfio_legacy_iommu_type;
static u32 vfio_iommufd_ioas_id;
static bool vfio_iommufd_ioas_valid;
static LIST_HEAD(vfio_groups);
static struct vfio_device *vfio_devices;

static bool vfio_use_iommufd(void)
{
	return vfio_backend == VFIO_BACKEND_IOMMUFD_COMPAT;
}

static bool vfio_cove_io_per_device_ioas(struct kvm *kvm)
{
	return kvm->cfg.cmode && vfio_use_iommufd();
}

static int vfio_iommufd_alloc_ioas(u32 *ioas_id);
static int vfio_iommufd_set_compat_ioas(u32 ioas_id);
static int vfio_iommufd_clear_compat_ioas(void);
static int vfio_iommufd_destroy_ioas(u32 ioas_id);
static int vfio_map_cvm_restricted_dma_device(struct kvm *kvm,
					      struct vfio_device *vdev);
static void vfio_unmap_cvm_restricted_dma_device(struct kvm *kvm,
						 struct vfio_device *vdev);

static int vfio_device_pci_parser(const struct option *opt, char *arg,
				  struct vfio_device_params *dev)
{
	unsigned int domain, bus, devnr, fn;

	int nr = sscanf(arg, "%4x:%2x:%2x.%1x", &domain, &bus, &devnr, &fn);
	if (nr < 4) {
		domain = 0;
		nr = sscanf(arg, "%2x:%2x.%1x", &bus, &devnr, &fn);
		if (nr < 3) {
			pr_err("Invalid device identifier %s", arg);
			return -EINVAL;
		}
	}

	dev->type = VFIO_DEVICE_PCI;
	dev->bus = "pci";
	dev->name = malloc(13);
	if (!dev->name)
		return -ENOMEM;

	snprintf(dev->name, 13, "%04x:%02x:%02x.%x", domain, bus, devnr, fn);
	dev->cove_io_pci_segment = domain;
	dev->cove_io_pci_requester_id =
		cove_io__pci_requester_id(bus, devnr, fn);
	dev->cove_io_device_id =
		cove_io__pci_device_id(domain, bus, devnr, fn);

	return 0;
}

int vfio_device_parser(const struct option *opt, const char *arg, int unset)
{
	int ret = -EINVAL;
	static int idx = 0;
	struct kvm *kvm = opt->ptr;
	struct vfio_device_params *dev, *devs;
	char *cur, *buf = strdup(arg);

	if (!buf)
		return -ENOMEM;

	if (idx >= MAX_VFIO_DEVICES) {
		pr_warning("Too many VFIO devices");
		goto out_free_buf;
	}

	devs = realloc(kvm->cfg.vfio_devices, sizeof(*dev) * (idx + 1));
	if (!devs) {
		ret = -ENOMEM;
		goto out_free_buf;
	}

	kvm->cfg.vfio_devices = devs;
	dev = &devs[idx];

	cur = strtok(buf, ",");
	if (!cur)
		goto out_free_buf;

	if (!strcmp(opt->long_name, "vfio-pci"))
		ret = vfio_device_pci_parser(opt, cur, dev);
	else
		ret = -EINVAL;

	if (!ret)
		kvm->cfg.num_vfio_devices = ++idx;

out_free_buf:
	free(buf);

	return ret;
}

static bool vfio_ioport_in(struct vfio_region *region, u32 offset,
			    void *data, int len)
{
	struct vfio_device *vdev = region->vdev;
	ssize_t nr;
	u32 val;

	if (!(region->info.flags & VFIO_REGION_INFO_FLAG_READ))
		return false;

	nr = pread(vdev->fd, &val, len, region->info.offset + offset);
	if (nr != len) {
		vfio_dev_err(vdev, "could not read %d bytes from I/O port 0x%x\n",
			     len, offset + region->port_base);
		return false;
	}

	switch (len) {
	case 1:
		ioport__write8(data, val);
		break;
	case 2:
		ioport__write16(data, val);
		break;
	case 4:
		ioport__write32(data, val);
		break;
	default:
		return false;
	}

	return true;
}

static bool vfio_ioport_out(struct vfio_region *region, u32 offset,
			     void *data, int len)
{
	struct vfio_device *vdev = region->vdev;
	ssize_t nr;
	u32 val;


	if (!(region->info.flags & VFIO_REGION_INFO_FLAG_WRITE))
		return false;

	switch (len) {
	case 1:
		val = ioport__read8(data);
		break;
	case 2:
		val = ioport__read16(data);
		break;
	case 4:
		val = ioport__read32(data);
		break;
	default:
		return false;
	}

	nr = pwrite(vdev->fd, &val, len, region->info.offset + offset);
	if (nr != len)
		vfio_dev_err(vdev, "could not write %d bytes to I/O port 0x%x",
			     len, offset + region->port_base);

	return nr == len;
}

static void vfio_ioport_mmio(struct kvm_cpu *vcpu, u64 addr, u8 *data, u32 len,
			     u8 is_write, void *ptr)
{
	struct vfio_region *region = ptr;
	u32 offset = addr - region->port_base;

	if (is_write)
		vfio_ioport_out(region, offset, data, len);
	else
		vfio_ioport_in(region, offset, data, len);
}

static void vfio_mmio_access(struct kvm_cpu *vcpu, u64 addr, u8 *data, u32 len,
			     u8 is_write, void *ptr)
{
	u64 val;
	ssize_t nr;
	struct vfio_region *region = ptr;
	struct vfio_device *vdev = region->vdev;

	u32 offset = addr - region->guest_phys_addr;

	if (len < 1 || len > 8)
		goto err_report;

	if (is_write) {
		if (!(region->info.flags & VFIO_REGION_INFO_FLAG_WRITE))
			goto err_report;

		memcpy(&val, data, len);

		nr = pwrite(vdev->fd, &val, len, region->info.offset + offset);
		if ((u32)nr != len)
			goto err_report;
	} else {
		if (!(region->info.flags & VFIO_REGION_INFO_FLAG_READ))
			goto err_report;

		nr = pread(vdev->fd, &val, len, region->info.offset + offset);
		if ((u32)nr != len)
			goto err_report;

		memcpy(data, &val, len);
	}

	return;

err_report:
	vfio_dev_err(vdev, "could not %s %u bytes at 0x%x (0x%llx)", is_write ?
		     "write" : "read", len, offset, addr);
}

static int vfio_setup_trap_region(struct kvm *kvm, struct vfio_device *vdev,
				  struct vfio_region *region)
{
	if (region->is_ioport) {
		int port;

		port = kvm__register_pio(kvm, region->port_base,
					 region->info.size, vfio_ioport_mmio,
					 region);
		if (port < 0)
			return port;
		return 0;
	}

	return kvm__register_mmio(kvm, region->guest_phys_addr,
				  region->info.size, false, vfio_mmio_access,
				  region);
}

int vfio_map_region(struct kvm *kvm, struct vfio_device *vdev,
		    struct vfio_region *region)
{
	void *base;
	int ret, prot = 0;
	/* KVM needs page-aligned regions */
	u64 map_size = ALIGN(region->info.size, PAGE_SIZE);

	if (!(region->info.flags & VFIO_REGION_INFO_FLAG_MMAP))
		return vfio_setup_trap_region(kvm, vdev, region);

	if (kvm->cfg.cmode)
		return vfio_setup_trap_region(kvm, vdev, region);

	/*
	 * KVM_SET_USER_MEMORY_REGION will fail because the guest physical
	 * address isn't page aligned, let's emulate the region ourselves.
	 */
	if (region->guest_phys_addr & (PAGE_SIZE - 1))
		return kvm__register_mmio(kvm, region->guest_phys_addr,
					  region->info.size, false,
					  vfio_mmio_access, region);

	if (region->info.flags & VFIO_REGION_INFO_FLAG_READ)
		prot |= PROT_READ;
	if (region->info.flags & VFIO_REGION_INFO_FLAG_WRITE)
		prot |= PROT_WRITE;

	base = mmap(NULL, region->info.size, prot, MAP_SHARED, vdev->fd,
		    region->info.offset);
	if (base == MAP_FAILED) {
		/* TODO: support sparse mmap */
		vfio_dev_warn(vdev, "failed to mmap region %u (0x%llx bytes), falling back to trapping",
			 region->info.index, region->info.size);
		return vfio_setup_trap_region(kvm, vdev, region);
	}
	region->host_addr = base;

	ret = kvm__register_dev_mem(kvm, region->guest_phys_addr, map_size,
				    region->host_addr);
	if (ret) {
		vfio_dev_err(vdev, "failed to register region with KVM");
		return ret;
	}

	return 0;
}

void vfio_unmap_region(struct kvm *kvm, struct vfio_region *region)
{
	u64 map_size;

	if (region->host_addr) {
		map_size = ALIGN(region->info.size, PAGE_SIZE);
		kvm__destroy_mem(kvm, region->guest_phys_addr, map_size,
				 region->host_addr);
		munmap(region->host_addr, region->info.size);
		region->host_addr = NULL;
	} else if (region->is_ioport) {
		kvm__deregister_pio(kvm, region->port_base);
	} else {
		kvm__deregister_mmio(kvm, region->guest_phys_addr);
	}
}

static int vfio_configure_device(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	struct vfio_group *group = vdev->group;

	if (vfio_cove_io_per_device_ioas(kvm)) {
		if (!vdev->iommufd_ioas_valid)
			return -EINVAL;

		ret = vfio_iommufd_set_compat_ioas(vdev->iommufd_ioas_id);
		if (ret)
			return ret;
		vfio_dev_info(vdev,
			      "selected COVE-IO iommufd IOAS %u before VFIO device open",
			      vdev->iommufd_ioas_id);
	}

	vdev->fd = ioctl(group->fd, VFIO_GROUP_GET_DEVICE_FD,
			 vdev->params->name);
	if (vdev->fd < 0) {
		ret = -errno;
		vfio_dev_warn(vdev, "failed to get fd: %s", strerror(errno));

		/* The device might be a bridge without an fd */
		if (vfio_use_iommufd())
			return ret;
		return 0;
	}

	vdev->info.argsz = sizeof(vdev->info);
	if (ioctl(vdev->fd, VFIO_DEVICE_GET_INFO, &vdev->info)) {
		ret = -errno;
		vfio_dev_err(vdev, "failed to get info");
		goto err_close_device;
	}

	if (vdev->info.flags & VFIO_DEVICE_FLAGS_RESET &&
	    ioctl(vdev->fd, VFIO_DEVICE_RESET) < 0)
		vfio_dev_warn(vdev, "failed to reset device");

	vdev->regions = calloc(vdev->info.num_regions, sizeof(*vdev->regions));
	if (!vdev->regions) {
		ret = -ENOMEM;
		goto err_close_device;
	}

	/* Now for the bus-specific initialization... */
	switch (vdev->params->type) {
	case VFIO_DEVICE_PCI:
		BUG_ON(!(vdev->info.flags & VFIO_DEVICE_FLAGS_PCI));
		ret = vfio_pci_setup_device(kvm, vdev);
		break;
	default:
		BUG_ON(1);
		ret = -EINVAL;
	}

	if (ret)
		goto err_free_regions;

	ret = vfio_map_cvm_restricted_dma_device(kvm, vdev);
	if (ret)
		goto err_teardown_device;

	vfio_dev_info(vdev, "assigned to device number 0x%x in group %lu",
		      vdev->dev_hdr.dev_num, group->id);

	return 0;

err_teardown_device:
	switch (vdev->params->type) {
	case VFIO_DEVICE_PCI:
		vfio_pci_teardown_device(kvm, vdev);
		break;
	default:
		break;
	}
err_free_regions:
	free(vdev->regions);
err_close_device:
	close(vdev->fd);
	vdev->fd = -1;

	return ret;
}

static int vfio_configure_devices(struct kvm *kvm)
{
	int i, ret;

	for (i = 0; i < kvm->cfg.num_vfio_devices; ++i) {
		ret = vfio_configure_device(kvm, &vfio_devices[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int vfio_get_iommu_type(void)
{
	if (ioctl(vfio_container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU))
		return VFIO_TYPE1v2_IOMMU;

	if (ioctl(vfio_container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU))
		return VFIO_TYPE1_IOMMU;

	return -ENODEV;
}

static int vfio_legacy_map_dma(struct kvm *kvm, u64 iova, void *vaddr,
			       u64 size, bool cove_io_lazy)
{
	int ret = 0;
	struct vfio_iommu_type1_dma_map dma_map = {
		.argsz	= sizeof(dma_map),
		.flags	= VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr	= (unsigned long)vaddr,
		.iova	= iova,
		.size	= size,
	};

	if (cove_io_lazy)
		dma_map.flags |= VFIO_DMA_MAP_FLAG_COVE_IO_LAZY;

	/* Map the guest memory for DMA (i.e. provide isolation) */
	if (ioctl(vfio_container, VFIO_IOMMU_MAP_DMA, &dma_map)) {
		ret = -errno;
		pr_err("Failed to map 0x%llx -> 0x%llx (%llu) for DMA",
		       dma_map.iova, dma_map.vaddr, dma_map.size);
	}

	return ret;
}

static int vfio_iommufd_map_dma_ioas(struct kvm *kvm, u32 ioas_id,
				     u64 iova, void *vaddr, u64 size,
				     bool cove_io_lazy)
{
	int ret = 0;
	struct iommu_ioas_map ioas_map = {
		.size		= sizeof(ioas_map),
		.flags		= IOMMU_IOAS_MAP_FIXED_IOVA |
				  IOMMU_IOAS_MAP_READABLE |
				  IOMMU_IOAS_MAP_WRITEABLE,
		.ioas_id	= ioas_id,
		.user_va	= (unsigned long)vaddr,
		.length		= size,
		.iova		= iova,
	};

	if (cove_io_lazy)
		ioas_map.flags |= IOMMU_IOAS_MAP_COVE_IO_LAZY;

	if (ioctl(vfio_container, IOMMU_IOAS_MAP, &ioas_map)) {
		ret = -errno;
		pr_err("Failed to iommufd map IOAS %u 0x%llx -> 0x%llx (%llu) for DMA",
		       ioas_id,
		       (unsigned long long)ioas_map.iova,
		       (unsigned long long)ioas_map.user_va,
		       (unsigned long long)ioas_map.length);
	}

	return ret;
}

static int vfio_iommufd_map_dma(struct kvm *kvm, u64 iova, void *vaddr,
				u64 size, bool cove_io_lazy)
{
	if (!vfio_iommufd_ioas_valid)
		return -EINVAL;

	return vfio_iommufd_map_dma_ioas(kvm, vfio_iommufd_ioas_id, iova,
					 vaddr, size, cove_io_lazy);
}

static int vfio_map_dma(struct kvm *kvm, u64 iova, void *vaddr, u64 size,
			bool cove_io_lazy)
{
	if (vfio_use_iommufd())
		return vfio_iommufd_map_dma(kvm, iova, vaddr, size,
					    cove_io_lazy);

	return vfio_legacy_map_dma(kvm, iova, vaddr, size, cove_io_lazy);
}

static int vfio_legacy_unmap_dma(struct kvm *kvm, u64 iova, u64 size)
{
	struct vfio_iommu_type1_dma_unmap dma_unmap = {
		.argsz = sizeof(dma_unmap),
		.size = size,
		.iova = iova,
	};

	ioctl(vfio_container, VFIO_IOMMU_UNMAP_DMA, &dma_unmap);

	return 0;
}

static int vfio_iommufd_unmap_dma_ioas(struct kvm *kvm, u32 ioas_id,
				       u64 iova, u64 size)
{
	int ret = 0;
	struct iommu_ioas_unmap ioas_unmap = {
		.size		= sizeof(ioas_unmap),
		.ioas_id	= ioas_id,
		.iova		= iova,
		.length		= size,
	};

	if (ioctl(vfio_container, IOMMU_IOAS_UNMAP, &ioas_unmap)) {
		ret = -errno;
		pr_warning("Failed to iommufd unmap IOAS %u 0x%llx (%llu) for DMA: %s",
			   ioas_id,
			   (unsigned long long)iova,
			   (unsigned long long)size,
			   strerror(errno));
	} else if (ioas_unmap.length != size) {
		pr_warning("iommufd unmapped IOAS %u 0x%llx partially: requested=%llu unmapped=%llu",
			   ioas_id,
			   (unsigned long long)iova,
			   (unsigned long long)size,
			   (unsigned long long)ioas_unmap.length);
	}

	return ret;
}

static int vfio_iommufd_unmap_dma(struct kvm *kvm, u64 iova, u64 size)
{
	if (!vfio_iommufd_ioas_valid)
		return -EINVAL;

	return vfio_iommufd_unmap_dma_ioas(kvm, vfio_iommufd_ioas_id, iova,
					   size);
}

static int vfio_unmap_dma(struct kvm *kvm, u64 iova, u64 size)
{
	if (vfio_use_iommufd())
		return vfio_iommufd_unmap_dma(kvm, iova, size);

	return vfio_legacy_unmap_dma(kvm, iova, size);
}

static int vfio_map_mem_bank(struct kvm *kvm, struct kvm_mem_bank *bank, void *data)
{
	return vfio_map_dma(kvm, bank->guest_phys_addr, bank->host_addr,
			    bank->size, false);
}

static int vfio_unmap_mem_bank(struct kvm *kvm, struct kvm_mem_bank *bank, void *data)
{
	return vfio_unmap_dma(kvm, bank->guest_phys_addr, bank->size);
}

static int vfio_map_cvm_restricted_dma(struct kvm *kvm)
{
#ifdef CONFIG_RISCV
	bool direct = cove_io__direct_dma_enabled();
	u64 iova = direct ? kvm->arch.memory_guest_start : CVM_VFIO_RMEM_ADDR;
	u64 size = direct ? kvm->ram_size : CVM_VFIO_RMEM_SIZE;
	void *host_addr = guest_flat_to_host(kvm, iova);
	bool lazy = cove_io__test_skip("vfio-iommu-map");

	if (!host_addr) {
		pr_err("Failed to find CVM restricted DMA pool at 0x%llx",
			(unsigned long long)iova);
		return -EINVAL;
	}

	pr_info("VFIO CVM DMA window: iova=0x%llx size=0x%llx",
		(unsigned long long)iova,
		(unsigned long long)size);
	if (lazy)
		pr_info("COVE-IO test: using lazy VFIO IOMMU DMA map for fault recovery");

	/* Direct-DMA is authorized page-by-page by the Monitor. Keep the VFIO
	 * range lazy so userspace VA pinning cannot pre-populate a conflicting
	 * host mapping before the Monitor returns the trusted HPA. */
	return vfio_map_dma(kvm, iova, host_addr, size, lazy || direct);
#else
	pr_err("VFIO CVM restricted DMA is only implemented for RISC-V");
	return -EINVAL;
#endif
}

static int vfio_unmap_cvm_restricted_dma(struct kvm *kvm)
{
#ifdef CONFIG_RISCV
	u64 iova = cove_io__direct_dma_enabled() ?
		kvm->arch.memory_guest_start : CVM_VFIO_RMEM_ADDR;
	u64 size = cove_io__direct_dma_enabled() ? kvm->ram_size : CVM_VFIO_RMEM_SIZE;
	return vfio_unmap_dma(kvm, iova, size);
#else
	return 0;
#endif
}

static int vfio_map_cvm_restricted_dma_device(struct kvm *kvm,
					      struct vfio_device *vdev)
{
#ifdef CONFIG_RISCV
	bool direct = cove_io__direct_dma_enabled();
	u64 iova = direct ? kvm->arch.memory_guest_start : CVM_VFIO_RMEM_ADDR;
	u64 size = direct ? kvm->ram_size : CVM_VFIO_RMEM_SIZE;
	void *host_addr = guest_flat_to_host(kvm, iova);
	bool lazy = cove_io__test_skip("vfio-iommu-map");
	int ret;

	if (!vfio_cove_io_per_device_ioas(kvm))
		return 0;
	if (!vdev->iommufd_ioas_valid)
		return -EINVAL;
	if (!host_addr) {
		pr_err("Failed to find CVM restricted DMA pool at 0x%llx",
			(unsigned long long)iova);
		return -EINVAL;
	}

	vfio_dev_info(vdev,
		      "VFIO CVM DMA window: IOAS %u iova=0x%llx size=0x%llx",
		      vdev->iommufd_ioas_id,
			      (unsigned long long)iova,
			      (unsigned long long)size);
	if (lazy)
		vfio_dev_info(vdev, "COVE-IO test: using lazy iommufd DMA map for fault recovery");

	if (direct) {
		vfio_dev_err(vdev,
			     "COVE_IO_DIRECT_DMA requires legacy VFIO type1; iommufd has no trusted-HPA insertion path");
		return -EOPNOTSUPP;
	}
	ret = vfio_iommufd_map_dma_ioas(kvm, vdev->iommufd_ioas_id,
					iova, host_addr, size, lazy);
	if (!ret)
		vdev->cove_io_dma_mapped = true;

	return ret;
#else
	pr_err("VFIO CVM restricted DMA is only implemented for RISC-V");
	return -EINVAL;
#endif
}

static void vfio_unmap_cvm_restricted_dma_device(struct kvm *kvm,
						 struct vfio_device *vdev)
{
#ifdef CONFIG_RISCV
	u64 iova = cove_io__direct_dma_enabled() ?
		kvm->arch.memory_guest_start : CVM_VFIO_RMEM_ADDR;
	u64 size = cove_io__direct_dma_enabled() ? kvm->ram_size : CVM_VFIO_RMEM_SIZE;
	if (!vfio_cove_io_per_device_ioas(kvm) || !vdev->cove_io_dma_mapped ||
	    !vdev->iommufd_ioas_valid)
		return;

	vfio_iommufd_unmap_dma_ioas(kvm, vdev->iommufd_ioas_id, iova, size);
	vdev->cove_io_dma_mapped = false;
#endif
}

static int vfio_configure_reserved_regions(struct kvm *kvm,
					   struct vfio_group *group)
{
	FILE *file;
	int ret = 0;
	char type[9];
	char filename[PATH_MAX];
	unsigned long long start, end;

	snprintf(filename, PATH_MAX, IOMMU_GROUP_DIR "/%lu/reserved_regions",
		 group->id);

	/* reserved_regions might not be present on older systems */
	if (access(filename, F_OK))
		return 0;

	file = fopen(filename, "r");
	if (!file)
		return -errno;

	while (fscanf(file, "0x%llx 0x%llx %8s\n", &start, &end, type) == 3) {
		ret = kvm__reserve_mem(kvm, start, end - start + 1);
		if (ret)
			break;
	}

	fclose(file);

	return ret;
}

static int vfio_configure_groups(struct kvm *kvm)
{
	int ret;
	struct vfio_group *group;

	list_for_each_entry(group, &vfio_groups, list) {
		ret = vfio_configure_reserved_regions(kvm, group);
		if (ret)
			return ret;
	}

	return 0;
}

static struct vfio_group *vfio_group_create(struct kvm *kvm, unsigned long id)
{
	int ret;
	struct vfio_group *group;
	char group_node[PATH_MAX];
	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status),
	};

	group = calloc(1, sizeof(*group));
	if (!group)
		return NULL;

	group->id	= id;
	group->refs	= 1;

	ret = snprintf(group_node, PATH_MAX, VFIO_DEV_DIR "/%lu", id);
	if (ret < 0 || ret == PATH_MAX)
		goto err_free_group;

	group->fd = open(group_node, O_RDWR);
	if (group->fd < 0) {
		pr_err("Failed to open IOMMU group %s", group_node);
		goto err_free_group;
	}

	if (ioctl(group->fd, VFIO_GROUP_GET_STATUS, &group_status)) {
		pr_err("Failed to determine status of IOMMU group %lu", id);
		goto err_close_group;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		pr_err("IOMMU group %lu is not viable", id);
		goto err_close_group;
	}

	if (ioctl(group->fd, VFIO_GROUP_SET_CONTAINER, &vfio_container)) {
		pr_err("Failed to add IOMMU group %lu to VFIO container", id);
		goto err_close_group;
	}

	list_add(&group->list, &vfio_groups);

	return group;

err_close_group:
	close(group->fd);
err_free_group:
	free(group);

	return NULL;
}

static void vfio_group_exit(struct kvm *kvm, struct vfio_group *group)
{
	if (--group->refs != 0)
		return;

	ioctl(group->fd, VFIO_GROUP_UNSET_CONTAINER);

	list_del(&group->list);
	close(group->fd);
	free(group);
}

static struct vfio_group *
vfio_group_get_for_dev(struct kvm *kvm, struct vfio_device *vdev)
{
	int dirfd;
	ssize_t ret;
	char *group_name;
	unsigned long group_id;
	char group_path[PATH_MAX];
	struct vfio_group *group = NULL;

	/* Find IOMMU group for this device */
	dirfd = open(vdev->sysfs_path, O_DIRECTORY | O_PATH | O_RDONLY);
	if (dirfd < 0) {
		vfio_dev_err(vdev, "failed to open '%s'", vdev->sysfs_path);
		return NULL;
	}

	ret = readlinkat(dirfd, "iommu_group", group_path, PATH_MAX);
	if (ret < 0) {
		vfio_dev_err(vdev, "no iommu_group");
		goto out_close;
	}
	if (ret == PATH_MAX)
		goto out_close;

	group_path[ret] = '\0';

	group_name = basename(group_path);
	errno = 0;
	group_id = strtoul(group_name, NULL, 10);
	if (errno)
		goto out_close;

	list_for_each_entry(group, &vfio_groups, list) {
		if (group->id == group_id) {
			if (vfio_cove_io_per_device_ioas(kvm)) {
				vfio_dev_err(vdev,
					     "COVE-IO iommufd compat requires one VFIO IOMMU group per device; group %lu is already used",
					     group_id);
				group = NULL;
				goto out_close;
			}
			group->refs++;
			goto out_close;
		}
	}

	group = vfio_group_create(kvm, group_id);

out_close:
	close(dirfd);
	return group;
}

static int vfio_device_init(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;
	char dev_path[PATH_MAX];
	struct vfio_group *group;

	vdev->fd = -1;

	ret = snprintf(dev_path, PATH_MAX, "/sys/bus/%s/devices/%s",
		       vdev->params->bus, vdev->params->name);
	if (ret < 0 || ret == PATH_MAX)
		return -EINVAL;

	vdev->sysfs_path = strndup(dev_path, PATH_MAX);
	if (!vdev->sysfs_path)
		return -errno;

	group = vfio_group_get_for_dev(kvm, vdev);
	if (!group) {
		free(vdev->sysfs_path);
		return -EINVAL;
	}

	vdev->group = group;

	if (vfio_cove_io_per_device_ioas(kvm)) {
		ret = vfio_iommufd_alloc_ioas(&vdev->iommufd_ioas_id);
		if (ret) {
			vfio_group_exit(kvm, group);
			free(vdev->sysfs_path);
			vdev->group = NULL;
			vdev->sysfs_path = NULL;
			return ret;
		}
		vdev->iommufd_ioas_valid = true;
		vfio_dev_info(vdev, "allocated COVE-IO iommufd IOAS %u",
			      vdev->iommufd_ioas_id);
	}

	return 0;
}

static void vfio_iommufd_destroy_device_ioas(struct vfio_device *vdev)
{
	if (!vdev->iommufd_ioas_valid)
		return;

	if (vfio_iommufd_ioas_valid)
		vfio_iommufd_set_compat_ioas(vfio_iommufd_ioas_id);
	else
		vfio_iommufd_clear_compat_ioas();

	vfio_iommufd_destroy_ioas(vdev->iommufd_ioas_id);
	vdev->iommufd_ioas_valid = false;
	vdev->iommufd_ioas_id = 0;
}

static void vfio_device_exit(struct kvm *kvm, struct vfio_device *vdev)
{
	vfio_unmap_cvm_restricted_dma_device(kvm, vdev);

	switch (vdev->params->type) {
	case VFIO_DEVICE_PCI:
		vfio_pci_teardown_device(kvm, vdev);
		break;
	default:
		vfio_dev_warn(vdev, "no teardown function for device");
	}

	if (vdev->fd >= 0) {
		close(vdev->fd);
		vdev->fd = -1;
	}

	vfio_iommufd_destroy_device_ioas(vdev);

	if (vdev->group)
		vfio_group_exit(kvm, vdev->group);

	free(vdev->regions);
	free(vdev->sysfs_path);
}

static int vfio_select_backend(void)
{
	const char *backend = getenv(COVE_IO_VFIO_BACKEND_ENV);

	vfio_backend = VFIO_BACKEND_LEGACY;

	if (!backend || !backend[0] || !strcmp(backend, "legacy") ||
	    !strcmp(backend, "type1"))
		return 0;
	if (cove_io__direct_dma_enabled() &&
	    (!strcmp(backend, "iommufd") || !strcmp(backend, "iommufd-compat"))) {
		pr_err("COVE_IO_DIRECT_DMA requires the legacy VFIO type1 backend");
		return -EOPNOTSUPP;
	}

	if (!strcmp(backend, "iommufd") ||
	    !strcmp(backend, "iommufd-compat")) {
		vfio_backend = VFIO_BACKEND_IOMMUFD_COMPAT;
		return 0;
	}

	pr_err("Unknown %s value '%s'", COVE_IO_VFIO_BACKEND_ENV, backend);
	return -EINVAL;
}

static int vfio_legacy_container_init(void)
{
	int api, iommu_type;

	vfio_container = open(VFIO_DEV_NODE, O_RDWR);
	if (vfio_container == -1) {
		pr_err("Failed to open %s", VFIO_DEV_NODE);
		return -errno;
	}

	api = ioctl(vfio_container, VFIO_GET_API_VERSION);
	if (api != VFIO_API_VERSION) {
		pr_err("Unknown VFIO API version %d", api);
		iommu_type = -ENODEV;
		goto err_close;
	}

	iommu_type = vfio_get_iommu_type();
	if (iommu_type < 0) {
		pr_err("VFIO type-1 IOMMU not supported on this platform");
		goto err_close;
	}

	vfio_legacy_iommu_type = iommu_type;
	return 0;

err_close:
	close(vfio_container);
	vfio_container = -1;
	return iommu_type;
}

static int vfio_iommufd_alloc_ioas(u32 *ioas_id)
{
	struct iommu_ioas_alloc alloc = {
		.size = sizeof(alloc),
	};

	if (ioctl(vfio_container, IOMMU_IOAS_ALLOC, &alloc)) {
		pr_err("Failed to allocate iommufd IOAS");
		return -errno;
	}

	*ioas_id = alloc.out_ioas_id;
	return 0;
}

static int vfio_iommufd_set_compat_ioas(u32 ioas_id)
{
	struct iommu_vfio_ioas vfio_ioas = {
		.size = sizeof(vfio_ioas),
		.op = IOMMU_VFIO_IOAS_SET,
		.ioas_id = ioas_id,
	};

	if (ioctl(vfio_container, IOMMU_VFIO_IOAS, &vfio_ioas)) {
		pr_err("Failed to set VFIO compatibility IOAS %u", ioas_id);
		return -errno;
	}

	return 0;
}

static int vfio_iommufd_clear_compat_ioas(void)
{
	struct iommu_vfio_ioas vfio_ioas = {
		.size = sizeof(vfio_ioas),
		.op = IOMMU_VFIO_IOAS_CLEAR,
	};

	if (ioctl(vfio_container, IOMMU_VFIO_IOAS, &vfio_ioas)) {
		pr_warning("Failed to clear VFIO compatibility IOAS: %s",
			   strerror(errno));
		return -errno;
	}

	return 0;
}

static int vfio_iommufd_destroy_ioas(u32 ioas_id)
{
	struct iommu_destroy destroy = {
		.size = sizeof(destroy),
		.id = ioas_id,
	};

	if (ioctl(vfio_container, IOMMU_DESTROY, &destroy)) {
		pr_warning("Failed to destroy iommufd IOAS %u: %s",
			   ioas_id, strerror(errno));
		return -errno;
	}

	return 0;
}

static int vfio_iommufd_compat_init(void)
{
	int ret;

	vfio_container = open(IOMMUFD_DEV_NODE, O_RDWR);
	if (vfio_container == -1) {
		pr_err("Failed to open %s", IOMMUFD_DEV_NODE);
		return -errno;
	}

	ret = vfio_iommufd_alloc_ioas(&vfio_iommufd_ioas_id);
	if (ret)
		goto err_close;

	vfio_iommufd_ioas_valid = true;

	ret = vfio_iommufd_set_compat_ioas(vfio_iommufd_ioas_id);
	if (ret)
		goto err_destroy_ioas;

	pr_info("Using iommufd compatibility default IOAS %u for VFIO",
		vfio_iommufd_ioas_id);
	return 0;

err_destroy_ioas:
	if (vfio_iommufd_ioas_valid) {
		vfio_iommufd_destroy_ioas(vfio_iommufd_ioas_id);
		vfio_iommufd_ioas_valid = false;
	}
err_close:
	close(vfio_container);
	vfio_container = -1;
	return ret;
}

static int vfio_backend_init(struct kvm *kvm)
{
	int ret;

	ret = vfio_select_backend();
	if (ret)
		return ret;

	if (vfio_use_iommufd())
		return vfio_iommufd_compat_init();

	return vfio_legacy_container_init();
}

static void vfio_backend_exit(void)
{
	if (vfio_container < 0)
		return;

	if (vfio_use_iommufd() && vfio_iommufd_ioas_valid) {
		vfio_iommufd_clear_compat_ioas();
		vfio_iommufd_destroy_ioas(vfio_iommufd_ioas_id);
		vfio_iommufd_ioas_valid = false;
		vfio_iommufd_ioas_id = 0;
	}

	close(vfio_container);
	vfio_container = -1;
}

static int vfio_container_init(struct kvm *kvm)
{
	int i, ret;

	ret = vfio_backend_init(kvm);
	if (ret)
		return ret;

	/* Create groups for our devices and add them to the container */
	for (i = 0; i < kvm->cfg.num_vfio_devices; ++i) {
		vfio_devices[i].params = &kvm->cfg.vfio_devices[i];

		ret = vfio_device_init(kvm, &vfio_devices[i]);
		if (ret)
			return ret;
	}

	/* Finalise the container */
	if (vfio_use_iommufd()) {
		pr_info("Using iommufd compatibility backend for VFIO");
	} else if (ioctl(vfio_container, VFIO_SET_IOMMU,
			 vfio_legacy_iommu_type)) {
		ret = -errno;
		pr_err("Failed to set IOMMU type %d for VFIO container",
		       vfio_legacy_iommu_type);
		return ret;
	} else {
		pr_info("Using IOMMU type %d for VFIO container",
			vfio_legacy_iommu_type);
	}

	if (kvm->cfg.cmode && !vfio_use_iommufd())
		return vfio_map_cvm_restricted_dma(kvm);

	if (kvm->cfg.cmode)
		return 0;

	return kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM,
				      vfio_map_mem_bank, NULL);
}

static int vfio__init(struct kvm *kvm)
{
	int ret;

	if (!kvm->cfg.num_vfio_devices)
		return 0;

	vfio_devices = calloc(kvm->cfg.num_vfio_devices, sizeof(*vfio_devices));
	if (!vfio_devices)
		return -ENOMEM;

	ret = vfio_container_init(kvm);
	if (ret)
		return ret;

	ret = vfio_configure_groups(kvm);
	if (ret)
		return ret;

	ret = vfio_configure_devices(kvm);
	if (ret)
		return ret;

	return 0;
}
dev_base_init(vfio__init);

static int vfio__exit(struct kvm *kvm)
{
	int i;

	if (!kvm->cfg.num_vfio_devices)
		return 0;

	for (i = 0; i < kvm->cfg.num_vfio_devices; i++)
		vfio_device_exit(kvm, &vfio_devices[i]);

	free(vfio_devices);

	if (kvm->cfg.cmode && !vfio_use_iommufd())
		vfio_unmap_cvm_restricted_dma(kvm);
	else if (!kvm->cfg.cmode)
		kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM,
				       vfio_unmap_mem_bank, NULL);
	vfio_backend_exit();

	free(kvm->cfg.vfio_devices);

	return 0;
}
dev_base_exit(vfio__exit);

u64 vfio__start_cove_io_tdis(struct kvm *kvm, u64 first_tdi_id)
{
	u64 tdi_id = first_tdi_id;
	int i;

	if (!kvm->cfg.num_vfio_devices)
		return tdi_id;

	for (i = 0; i < kvm->cfg.num_vfio_devices; i++) {
		switch (vfio_devices[i].params->type) {
		case VFIO_DEVICE_PCI:
			tdi_id = vfio_pci_start_cove_io_tdi(kvm,
							    &vfio_devices[i],
							    tdi_id);
			break;
		default:
			break;
		}
	}

	return tdi_id;
}

void vfio__probe_cove_io_tdis(struct kvm *kvm)
{
	int i;

	if (!kvm->cfg.num_vfio_devices)
		return;

	for (i = 0; i < kvm->cfg.num_vfio_devices; i++) {
		switch (vfio_devices[i].params->type) {
		case VFIO_DEVICE_PCI:
			vfio_pci_probe_cove_io_tdi(kvm, &vfio_devices[i]);
			break;
		default:
			break;
		}
	}
}

u64 vfio__first_cove_io_device_id(struct kvm *kvm)
{
	int i;

	if (!vfio_devices)
		return KVM_COVE_IO_DEVICE_ANY;

	for (i = 0; i < kvm->cfg.num_vfio_devices; i++) {
		if (vfio_devices[i].params)
			return vfio_devices[i].params->cove_io_device_id;
	}

	return KVM_COVE_IO_DEVICE_ANY;
}

u64 vfio__first_cove_io_wrong_rid_device_id(struct kvm *kvm)
{
	int i;

	if (!vfio_devices)
		return KVM_COVE_IO_DEVICE_ANY;

	for (i = 0; i < kvm->cfg.num_vfio_devices; i++) {
		struct vfio_device_params *params;

		params = vfio_devices[i].params;
		if (params)
			return cove_io__pci_device_id_from_rid(
				params->cove_io_pci_segment,
				params->cove_io_pci_requester_id ^ 0x1);
	}

	return KVM_COVE_IO_DEVICE_ANY;
}

u64 vfio__first_cove_io_iommu_group(struct kvm *kvm)
{
	int i;

	if (!vfio_devices)
		return COVE_IO_IOMMU_GROUP_NONE;

	for (i = 0; i < kvm->cfg.num_vfio_devices; i++) {
		if (vfio_devices[i].params && vfio_devices[i].group)
			return vfio_devices[i].group->id;
	}

	return COVE_IO_IOMMU_GROUP_NONE;
}

void vfio__register_cove_io_swiotlb(struct kvm *kvm, u64 addr, u64 size)
{
	int i;

	if (!vfio_devices)
		return;

	for (i = 0; i < kvm->cfg.num_vfio_devices; i++) {
		struct vfio_device *vdev = &vfio_devices[i];
		struct swiotlb sw;

		if (!vdev->params || !vdev->group)
			continue;

		sw = (struct swiotlb) {
			.addr = addr,
			.size = size,
			.device_id = vdev->params->cove_io_device_id,
			.iommu_group = vdev->group->id,
			.flags = cove_io__direct_dma_enabled() ?
				KVM_COVE_IO_SWIOTLB_F_IOMMU_ONLY : 0,
		};

		if (i == 0) {
			if (cove_io__test_bad_swiotlb_device("vfio"))
				sw.device_id = COVE_IO_DEVICE_TEST_WRONG;
			else if (cove_io__test_bad_swiotlb_rid("vfio"))
				sw.device_id = vfio__first_cove_io_wrong_rid_device_id(kvm);
			if (cove_io__test_bad_swiotlb_iommu_group("vfio"))
				sw.iommu_group++;
		}

		if (ioctl(kvm->vm_fd, KVM_SET_SWIOTLB, &sw))
			die_perror("KVM_SET_SWIOTLB VFIO ioctl");
	}
}
