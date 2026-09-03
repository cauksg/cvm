#include "kvm/cove-io.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <limits.h>

#define COVE_IO_TEST_SKIP_ENV	"COVE_IO_TEST_SKIP"
#define COVE_IO_TEST_PROBE_ENV	"COVE_IO_TEST_PROBE"
#define COVE_IO_TEST_BAD_SWIOTLB_DEVICE_ENV "COVE_IO_TEST_BAD_SWIOTLB_DEVICE"
#define COVE_IO_TEST_BAD_SWIOTLB_RID_ENV "COVE_IO_TEST_BAD_SWIOTLB_RID"
#define COVE_IO_TEST_BAD_SWIOTLB_IOMMU_GROUP_ENV "COVE_IO_TEST_BAD_SWIOTLB_IOMMU_GROUP"
#define COVE_IO_TEST_BAD_TDI_RID_ENV "COVE_IO_TEST_BAD_TDI_RID"
#define COVE_IO_TEST_BAD_IOMMU_GROUP_ENV "COVE_IO_TEST_BAD_IOMMU_GROUP"
#define COVE_IO_TEST_PAUSE_BEFORE_RUN_ENV "COVE_IO_TEST_PAUSE_BEFORE_RUN"
#define COVE_IO_TEST_MRIF_RETARGET_LOOPS_ENV "COVE_IO_TEST_MRIF_RETARGET_LOOPS"
#define COVE_IO_DIRECT_DMA_ENV "COVE_IO_DIRECT_DMA"
#define COVE_IO_LIFECYCLE_TEST_ENV "COVE_IO_TEST_LIFECYCLE"

bool cove_io__test_skip(const char *name)
{
	const char *env = getenv(COVE_IO_TEST_SKIP_ENV);
	size_t name_len;
	const char *p;

	if (!env || !*env)
		return false;

	name_len = strlen(name);
	p = env;
	while (*p) {
		size_t token_len;

		while (*p == ',' || *p == ':' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;

		token_len = strcspn(p, ",: \t");
		if (token_len == name_len && !strncmp(p, name, name_len))
			return true;

		p += token_len;
	}

	return false;
}

bool cove_io__test_probe_enabled(void)
{
	const char *env = getenv(COVE_IO_TEST_PROBE_ENV);

	return env && strcmp(env, "0");
}

bool cove_io__test_bad_swiotlb_device(const char *name)
{
	const char *env = getenv(COVE_IO_TEST_BAD_SWIOTLB_DEVICE_ENV);

	return env && !strcmp(env, name);
}

bool cove_io__test_bad_swiotlb_rid(const char *name)
{
	const char *env = getenv(COVE_IO_TEST_BAD_SWIOTLB_RID_ENV);

	return env && !strcmp(env, name);
}

bool cove_io__test_bad_swiotlb_iommu_group(const char *name)
{
	const char *env = getenv(COVE_IO_TEST_BAD_SWIOTLB_IOMMU_GROUP_ENV);

	return env && !strcmp(env, name);
}

bool cove_io__test_bad_tdi_rid(const char *name)
{
	const char *env = getenv(COVE_IO_TEST_BAD_TDI_RID_ENV);

	return env && !strcmp(env, name);
}

bool cove_io__test_bad_iommu_group(const char *name)
{
	const char *env = getenv(COVE_IO_TEST_BAD_IOMMU_GROUP_ENV);

	return env && !strcmp(env, name);
}

unsigned int cove_io__test_pause_before_run_secs(void)
{
	const char *env = getenv(COVE_IO_TEST_PAUSE_BEFORE_RUN_ENV);
	char *end;
	unsigned long secs;

	if (!env || !*env)
		return 0;

	secs = strtoul(env, &end, 0);
	if (end == env || *end || secs > UINT_MAX)
		return 0;

	return secs;
}

unsigned int cove_io__test_mrif_retarget_loops(void)
{
	const char *env = getenv(COVE_IO_TEST_MRIF_RETARGET_LOOPS_ENV);
	char *end;
	unsigned long loops;

	if (!env || !*env)
		return 1;

	loops = strtoul(env, &end, 0);
	if (end == env || *end || loops == 0 || loops > UINT_MAX)
		return 1;

	return loops;
}

bool cove_io__direct_dma_enabled(void)
{
	const char *env = getenv(COVE_IO_DIRECT_DMA_ENV);

	return env && strcmp(env, "0");
}

bool cove_io__lifecycle_test_enabled(void)
{
	const char *env = getenv(COVE_IO_LIFECYCLE_TEST_ENV);

	return env && strcmp(env, "0");
}

static bool cove_io__device_uses_direct_dma(u64 device_id)
{
	u64 type = (device_id & KVM_COVE_IO_DEVICE_TYPE_MASK) >>
		   KVM_COVE_IO_DEVICE_TYPE_SHIFT;

	return cove_io__direct_dma_enabled() &&
	       type == KVM_COVE_IO_DEVICE_TYPE_PCI_RID;
}

static const char *cove_io__tdi_op_name(u32 op)
{
	switch (op) {
	case KVM_COVE_IO_TDI_REGISTER:
		return "REGISTER";
	case KVM_COVE_IO_TDI_UNREGISTER:
		return "UNREGISTER";
	case KVM_COVE_IO_TDI_ADD_MMIO:
		return "ADD_MMIO";
	case KVM_COVE_IO_TDI_RECLAIM_MMIO:
		return "RECLAIM_MMIO";
	case KVM_COVE_IO_TDI_BIND:
		return "BIND";
	case KVM_COVE_IO_TDI_UNBIND:
		return "UNBIND";
	case KVM_COVE_IO_TDI_DMA_MAP:
		return "DMA_MAP";
	case KVM_COVE_IO_TDI_DMA_UNMAP:
		return "DMA_UNMAP";
	case KVM_COVE_IO_TDI_IRQ_BIND:
		return "IRQ_BIND";
	case KVM_COVE_IO_TDI_IRQ_UNBIND:
		return "IRQ_UNBIND";
	case KVM_COVE_IO_TDI_ACCEPT_START:
		return "ACCEPT_START";
	case KVM_COVE_IO_TDI_STOP:
		return "STOP";
	case KVM_COVE_IO_TDI_GET_STATE:
		return "GET_STATE";
	case KVM_COVE_IO_TDI_FIND_DMA:
		return "FIND_DMA";
	case KVM_COVE_IO_TDI_FIND_IRQ:
		return "FIND_IRQ";
	case KVM_COVE_IO_TDI_FIND_MMIO:
		return "FIND_MMIO";
	case KVM_COVE_IO_TDI_GET_FEATURES:
		return "GET_FEATURES";
	case KVM_COVE_IO_TDI_SET_ERROR:
		return "SET_ERROR";
	case KVM_COVE_IO_TDI_FINALIZE_STOP:
		return "FINALIZE_STOP";
	case KVM_COVE_IO_TDI_ENUM_OWNED:
		return "ENUM_OWNED";
	default:
		return "UNKNOWN";
	}
}

void cove_io__tdi_op(struct kvm *kvm, struct kvm_cove_io_tdi *tdi)
{
	struct kvm_cove_io_tdi state;
	int err;

	if (tdi->op != KVM_COVE_IO_TDI_REGISTER &&
	    tdi->op != KVM_COVE_IO_TDI_GET_FEATURES &&
	    tdi->op != KVM_COVE_IO_TDI_FIND_MMIO &&
	    tdi->op != KVM_COVE_IO_TDI_FIND_DMA &&
	    tdi->op != KVM_COVE_IO_TDI_FIND_IRQ &&
	    tdi->op != KVM_COVE_IO_TDI_ENUM_OWNED) {
		if (!(tdi->flags & KVM_COVE_IO_TDI_F_EXPECT_GENERATION)) {
			memset(&state, 0, sizeof(state));
			state.op = KVM_COVE_IO_TDI_GET_STATE;
			state.tdi_id = tdi->tdi_id;
			if (ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &state))
				die("KVM_COVE_IO_TDI_OP generation query failed: tdi=%llu: %s",
				    (unsigned long long)tdi->tdi_id, strerror(errno));
			tdi->generation = state.generation;
		}
		tdi->flags |= KVM_COVE_IO_TDI_F_EXPECT_GENERATION;
	}

	if (!ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, tdi))
		return;

	err = errno;
	die("KVM_COVE_IO_TDI_OP ioctl failed: op=%s(%u) tdi=%llu generation=%llu flags=0x%x state=%llu "
	    "device_id=0x%llx iommu_group=%llu mmio=0x%llx+0x%llx "
	    "dma=0x%llx+0x%llx irq=%llu+%llu vcpu=%llu iid=0x%llx: %s",
	    cove_io__tdi_op_name(tdi->op), tdi->op,
	    (unsigned long long)tdi->tdi_id,
	    (unsigned long long)tdi->generation, tdi->flags,
	    (unsigned long long)tdi->state,
	    (unsigned long long)tdi->device_id,
	    (unsigned long long)tdi->iommu_group,
	    (unsigned long long)tdi->mmio_gpa,
	    (unsigned long long)tdi->mmio_size,
	    (unsigned long long)tdi->dma_gpa,
	    (unsigned long long)tdi->dma_size,
	    (unsigned long long)tdi->irq_id,
	    (unsigned long long)tdi->irq_num,
	    (unsigned long long)tdi->vcpu_id,
	    (unsigned long long)tdi->irq_iid,
	    strerror(err));
}

bool cove_io__probe_tdi_op(struct kvm *kvm, struct kvm_cove_io_tdi *tdi)
{
	tdi->flags |= KVM_COVE_IO_TDI_F_ALLOW_BOUND_PROBE;
	return ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, tdi) == 0 &&
	       (tdi->state == KVM_COVE_IO_TDI_STATE_STARTED ||
		tdi->state == KVM_COVE_IO_TDI_STATE_BOUND);
}

void cove_io__expect_probe(struct kvm *kvm, struct kvm_cove_io_tdi tdi,
			   bool expect_allowed, const char *name)
{
	struct kvm_cove_io_tdi runtime = tdi;
	bool allowed = cove_io__probe_tdi_op(kvm, &tdi);
	bool runtime_allowed;

	runtime.flags &= ~KVM_COVE_IO_TDI_F_ALLOW_BOUND_PROBE;
	runtime_allowed = ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &runtime) == 0 &&
			  runtime.state == KVM_COVE_IO_TDI_STATE_STARTED;
	if (runtime_allowed)
		die("COVE-IO pre-accept runtime probe unexpectedly allowed: %s",
		    name);

	if (allowed != expect_allowed)
		die("COVE-IO probe failed: %s expected %s, got %s",
		    name, expect_allowed ? "allow" : "deny",
		    allowed ? "allow" : "deny");

	pr_info("COVE-IO probe %s: configured=%s runtime-before-accept=deny",
		name, allowed ? "allow" : "deny");
}

u64 cove_io__start_tdi(struct kvm *kvm, u64 tdi_id, u64 mmio_gpa,
		       u64 mmio_size, u64 device_id, u64 dma_gpa,
		       u64 dma_size, u64 iommu_group, u64 irq_id,
		       u64 irq_num)
{
	struct kvm_cove_io_tdi tdi = {
		.tdi_id = tdi_id,
		.iommu_group = COVE_IO_IOMMU_GROUP_NONE,
	};

	if (tdi_id == COVE_IO_DEFAULT_TDI_ID) {
		struct kvm_cove_io_tdi features = {
			.op = KVM_COVE_IO_TDI_GET_FEATURES,
		};

		cove_io__tdi_op(kvm, &features);
		pr_info("COVE-IO features=0x%llx%s%s",
			(unsigned long long)features.features,
			(features.features & KVM_COVE_IO_FEAT_COVG_START_STOP) ?
			" covg-start-stop" : "",
			(features.features & KVM_COVE_IO_FEAT_DIRECT_DMA) ?
			" direct-dma" : "");
	}

	tdi.op = KVM_COVE_IO_TDI_REGISTER;
	tdi.flags = KVM_COVE_IO_TDI_F_AUTO_ID;
	cove_io__tdi_op(kvm, &tdi);
	tdi_id = tdi.tdi_id;
	if (cove_io__test_probe_enabled()) {
		struct kvm_cove_io_tdi stale = {
			.op = KVM_COVE_IO_TDI_BIND,
			.flags = KVM_COVE_IO_TDI_F_EXPECT_GENERATION,
			.tdi_id = tdi_id,
			.generation = tdi.generation ^ 1,
		};

		if (!ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &stale))
			die("COVE-IO stale generation unexpectedly accepted: tdi=%llu",
			    (unsigned long long)tdi_id);
		pr_info("COVE-IO stale generation denied: tdi=%llu",
			(unsigned long long)tdi_id);
	}
	if (mmio_size) {
		tdi.op = KVM_COVE_IO_TDI_ADD_MMIO;
		tdi.mmio_gpa = mmio_gpa;
		tdi.mmio_size = mmio_size;
		cove_io__tdi_op(kvm, &tdi);
	}

	tdi.op = KVM_COVE_IO_TDI_BIND;
	cove_io__tdi_op(kvm, &tdi);

	if (dma_size) {
		tdi.op = KVM_COVE_IO_TDI_DMA_MAP;
		if (cove_io__device_uses_direct_dma(device_id))
			tdi.flags |= KVM_COVE_IO_TDI_F_DIRECT_DMA;
		tdi.device_id = device_id;
		tdi.iommu_group = iommu_group;
		tdi.dma_gpa = dma_gpa;
		tdi.dma_size = dma_size;
		cove_io__tdi_op(kvm, &tdi);
	}

	if (irq_num) {
		tdi.op = KVM_COVE_IO_TDI_IRQ_BIND;
		tdi.irq_id = irq_id;
		tdi.irq_num = irq_num;
		tdi.vcpu_id = 0;
		tdi.irq_iid = COVE_IO_IRQ_IID_ANY;
		cove_io__tdi_op(kvm, &tdi);
	}

	/* Guest COVG start-interface performs the acceptance transition. */
	pr_info("COVE-IO TDI %llu bound; waiting for guest COVG acceptance",
		(unsigned long long)tdi_id);

	return tdi_id;
}

void cove_io__bind_irq(struct kvm *kvm, u64 tdi_id, u64 irq_id, u64 irq_num)
{
	cove_io__bind_irq_target(kvm, tdi_id, irq_id, irq_num, 0);
}

void cove_io__bind_irq_target(struct kvm *kvm, u64 tdi_id, u64 irq_id,
			      u64 irq_num, u64 vcpu_id)
{
	cove_io__bind_irq_target_iid(kvm, tdi_id, irq_id, irq_num, vcpu_id,
				     COVE_IO_IRQ_IID_ANY);
}

void cove_io__bind_irq_target_iid(struct kvm *kvm, u64 tdi_id, u64 irq_id,
				  u64 irq_num, u64 vcpu_id, u64 irq_iid)
{
	cove_io__bind_irq_target_iid_device(kvm, tdi_id, irq_id, irq_num,
					    vcpu_id, irq_iid,
					    KVM_COVE_IO_DEVICE_ANY);
}

void cove_io__bind_irq_target_iid_device(struct kvm *kvm, u64 tdi_id,
					 u64 irq_id, u64 irq_num,
					 u64 vcpu_id, u64 irq_iid,
					 u64 device_id)
{
	struct kvm_cove_io_tdi tdi = {
		.op = KVM_COVE_IO_TDI_IRQ_BIND,
		.tdi_id = tdi_id,
		.irq_id = irq_id,
		.irq_num = irq_num,
		.vcpu_id = vcpu_id,
		.irq_iid = irq_iid,
		.device_id = device_id,
	};

	cove_io__tdi_op(kvm, &tdi);
}

void cove_io__unbind_irq(struct kvm *kvm, u64 tdi_id)
{
	struct kvm_cove_io_tdi tdi = {
		.op = KVM_COVE_IO_TDI_IRQ_UNBIND,
		.tdi_id = tdi_id,
	};

	cove_io__tdi_op(kvm, &tdi);
}

void cove_io__unbind_irq_range(struct kvm *kvm, u64 tdi_id, u64 irq_id,
			       u64 irq_num)
{
	struct kvm_cove_io_tdi tdi = {
		.op = KVM_COVE_IO_TDI_IRQ_UNBIND,
		.tdi_id = tdi_id,
		.irq_id = irq_id,
		.irq_num = irq_num,
	};

	cove_io__tdi_op(kvm, &tdi);
}

bool cove_io__try_reclaim_mmio(struct kvm *kvm, u64 tdi_id)
{
	struct kvm_cove_io_tdi tdi = {
		.op = KVM_COVE_IO_TDI_GET_STATE,
		.tdi_id = tdi_id,
	};

	if (ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi))
		return false;
	tdi.op = KVM_COVE_IO_TDI_RECLAIM_MMIO;
	tdi.flags |= KVM_COVE_IO_TDI_F_EXPECT_GENERATION;
	return ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi) == 0;
}

bool cove_io__try_unmap_dma(struct kvm *kvm, u64 tdi_id)
{
	struct kvm_cove_io_tdi tdi = {
		.op = KVM_COVE_IO_TDI_GET_STATE,
		.tdi_id = tdi_id,
	};

	if (ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi))
		return false;
	tdi.op = KVM_COVE_IO_TDI_DMA_UNMAP;
	tdi.flags |= KVM_COVE_IO_TDI_F_EXPECT_GENERATION;
	return ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi) == 0;
}

void cove_io__teardown_tdi(struct kvm *kvm, u64 tdi_id)
{
	struct kvm_cove_io_tdi tdi = {
		.tdi_id = tdi_id,
	};
	struct kvm_cove_io_tdi initial;
	struct kvm_cove_io_tdi probe;
	u64 initial_generation;

	if (!tdi_id || kvm->vm_fd < 0)
		return;

	tdi.op = KVM_COVE_IO_TDI_GET_STATE;
	if (ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi) < 0)
		return;
	initial = tdi;
	initial_generation = tdi.generation;

	if (tdi.state == KVM_COVE_IO_TDI_STATE_STARTED ||
	    tdi.state == KVM_COVE_IO_TDI_STATE_BOUND) {
		tdi.op = KVM_COVE_IO_TDI_STOP;
		tdi.flags |= KVM_COVE_IO_TDI_F_EXPECT_GENERATION;
		if (ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi) < 0) {
			pr_warning("COVE-IO STOP failed during teardown: tdi=%llu: %s",
				   (unsigned long long)tdi_id, strerror(errno));
			return;
		}
		tdi.state = KVM_COVE_IO_TDI_STATE_STOPPING;
	}

	if (tdi.state != KVM_COVE_IO_TDI_STATE_STOPPING &&
	    tdi.state != KVM_COVE_IO_TDI_STATE_ERROR)
		return;

	if (initial.mmio_size) {
		memset(&probe, 0, sizeof(probe));
		probe.op = KVM_COVE_IO_TDI_FIND_MMIO;
		probe.mmio_gpa = initial.mmio_gpa;
		probe.mmio_size = 1;
		if (!ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &probe))
			pr_warning("COVE-IO teardown MMIO revoke failed: tdi=%llu",
				   (unsigned long long)tdi_id);
	}
	if (initial.dma_size) {
		memset(&probe, 0, sizeof(probe));
		probe.op = KVM_COVE_IO_TDI_FIND_DMA;
		probe.device_id = initial.device_id;
		probe.dma_gpa = initial.dma_gpa;
		if (!ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &probe))
			pr_warning("COVE-IO teardown DMA revoke failed: tdi=%llu",
				   (unsigned long long)tdi_id);
	}
	if (initial.irq_num) {
		memset(&probe, 0, sizeof(probe));
		probe.op = KVM_COVE_IO_TDI_FIND_IRQ;
		probe.irq_id = initial.irq_id;
		probe.vcpu_id = initial.vcpu_id;
		probe.irq_iid = initial.irq_iid;
		probe.device_id = initial.device_id;
		if (!ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &probe))
			pr_warning("COVE-IO teardown IRQ revoke failed: tdi=%llu",
				   (unsigned long long)tdi_id);
	}

	tdi.op = KVM_COVE_IO_TDI_IRQ_UNBIND;
	tdi.flags |= KVM_COVE_IO_TDI_F_EXPECT_GENERATION;
	tdi.irq_id = 0;
	tdi.irq_num = 0;
	ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi);

	cove_io__try_unmap_dma(kvm, tdi_id);
	cove_io__try_reclaim_mmio(kvm, tdi_id);

	tdi.op = KVM_COVE_IO_TDI_FINALIZE_STOP;
	tdi.flags |= KVM_COVE_IO_TDI_F_EXPECT_GENERATION;
	if (ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi) < 0) {
		pr_warning("COVE-IO FINALIZE_STOP failed: tdi=%llu: %s",
			   (unsigned long long)tdi_id, strerror(errno));
		return;
	}
	if (tdi.state != KVM_COVE_IO_TDI_STATE_REGISTERED ||
	    tdi.generation != initial_generation + 1 || tdi.mmio_size ||
	    tdi.dma_size || tdi.irq_num ||
	    tdi.device_id != KVM_COVE_IO_DEVICE_ANY) {
		pr_warning("COVE-IO final state validation failed: tdi=%llu state=%llu generation=%llu",
			   (unsigned long long)tdi_id,
			   (unsigned long long)tdi.state,
			   (unsigned long long)tdi.generation);
		return;
	}

	probe = tdi;
	probe.op = KVM_COVE_IO_TDI_UNREGISTER;
	probe.generation = initial_generation;
	probe.flags |= KVM_COVE_IO_TDI_F_EXPECT_GENERATION;
	if (!ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &probe)) {
		pr_warning("COVE-IO stale teardown generation accepted: tdi=%llu",
			   (unsigned long long)tdi_id);
		return;
	}

	tdi.op = KVM_COVE_IO_TDI_UNREGISTER;
	tdi.flags |= KVM_COVE_IO_TDI_F_EXPECT_GENERATION;
	if (ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi) < 0 ||
	    tdi.state != KVM_COVE_IO_TDI_STATE_FREE ||
	    tdi.generation != initial_generation + 2) {
		pr_warning("COVE-IO UNREGISTER validation failed: tdi=%llu",
			   (unsigned long long)tdi_id);
		return;
	}

	pr_info("COVE-IO teardown verified: tdi=%llu generation=%llu->%llu state=free runtime=revoked",
		(unsigned long long)tdi_id,
		(unsigned long long)initial_generation,
		(unsigned long long)tdi.generation);
}

void cove_io__teardown_all(struct kvm *kvm)
{
	struct kvm_cove_io_tdi tdi;
	u64 cursor = 0;
	unsigned int count;

	for (count = 0; count < COVE_IO_MAX_TDIS; count++) {
		memset(&tdi, 0, sizeof(tdi));
		tdi.op = KVM_COVE_IO_TDI_ENUM_OWNED;
		tdi.tdi_id = cursor;
		if (ioctl(kvm->vm_fd, KVM_COVE_IO_TDI_OP, &tdi) < 0) {
			pr_warning("COVE-IO owner enumeration failed after tdi=%llu: %s",
				   (unsigned long long)cursor, strerror(errno));
			return;
		}
		if (!tdi.tdi_id)
			return;
		if (tdi.tdi_id <= cursor) {
			pr_warning("COVE-IO owner enumeration did not advance: cursor=%llu next=%llu",
				   (unsigned long long)cursor,
				   (unsigned long long)tdi.tdi_id);
			return;
		}
		cursor = tdi.tdi_id;
		cove_io__teardown_tdi(kvm, cursor);
	}

	pr_warning("COVE-IO owner enumeration exceeded %u interfaces",
		   COVE_IO_MAX_TDIS);
}
