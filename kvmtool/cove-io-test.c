#include "kvm/cove-io.h"
#include "kvm/kvm.h"
#include "kvm/util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __riscv
static void test_op(int vm_fd, struct kvm_cove_io_tdi *request,
		    bool success, const char *name)
{
	int result = ioctl(vm_fd, KVM_COVE_IO_TDI_OP, request);

	if ((result == 0) != success)
		die("FAIL: resource %s result=%d errno=%d", name, result, errno);
}

static void test_irq(int vm_fd, u64 tdi_id, u64 irq, bool allowed)
{
	struct kvm_cove_io_tdi request = {
		.op = KVM_COVE_IO_TDI_FIND_IRQ,
		.flags = KVM_COVE_IO_TDI_F_ALLOW_BOUND_PROBE,
		.tdi_id = tdi_id,
		.irq_id = irq,
		.irq_iid = 11,
	};

	test_op(vm_fd, &request, allowed, "IRQ authorization");
}

static void test_irq_capacity(int vm_fd, u64 tdi_id)
{
	struct kvm_cove_io_tdi request;
	unsigned int index;

	for (index = 0; index < 256; index++) {
		request = (struct kvm_cove_io_tdi) {
			.op = KVM_COVE_IO_TDI_IRQ_BIND,
			.tdi_id = tdi_id,
			.irq_id = 0x200000 + index * 8,
			.irq_num = 3,
			.irq_iid = 11,
		};
		test_op(vm_fd, &request, true, "IRQ fill");
	}
	request = (struct kvm_cove_io_tdi) {
		.op = KVM_COVE_IO_TDI_IRQ_BIND,
		.tdi_id = tdi_id,
		.irq_id = 0x200001,
		.irq_num = 1,
		.irq_iid = 12,
	};
	test_op(vm_fd, &request, false, "full IRQ split bind");
	request.op = KVM_COVE_IO_TDI_IRQ_UNBIND;
	test_op(vm_fd, &request, false, "full IRQ split unbind");
	for (index = 0; index < 256; index++) {
		test_irq(vm_fd, tdi_id, 0x200000 + index * 8, true);
		test_irq(vm_fd, tdi_id, 0x200001 + index * 8, true);
		test_irq(vm_fd, tdi_id, 0x200002 + index * 8, true);
	}
	request.irq_id = 0x200000 + 255 * 8;
	request.irq_num = 3;
	test_op(vm_fd, &request, true, "IRQ release slot");
	request.op = KVM_COVE_IO_TDI_IRQ_UNBIND;
	request.irq_id = 0x200001;
	request.irq_num = 1;
	test_op(vm_fd, &request, true, "IRQ split retry");
	test_irq(vm_fd, tdi_id, 0x200000, true);
	test_irq(vm_fd, tdi_id, 0x200001, false);
	test_irq(vm_fd, tdi_id, 0x200002, true);
	pr_info("CHECK: IRQ capacity=256 failed bind/unbind preserved every old range; retry passed");
}

static void test_vm_capacity(struct kvm *kvm)
{
	int descriptors[31];
	unsigned int index, round;
	int extra;

	for (round = 0; round < 2; round++) {
		for (index = 0; index < ARRAY_SIZE(descriptors); index++) {
			descriptors[index] = ioctl(kvm->sys_fd, KVM_CREATE_VM, CVM_VM_TYPE);
			if (descriptors[index] < 0)
				die("FAIL: CVM capacity round=%u index=%u errno=%d", round, index, errno);
		}
		for (index = 0; index < 8; index++) {
			extra = ioctl(kvm->sys_fd, KVM_CREATE_VM, CVM_VM_TYPE);
			if (extra >= 0 || errno != ENOMEM)
				die("FAIL: full CVM pool result=%d errno=%d", extra, errno);
		}
		for (index = 0; index < ARRAY_SIZE(descriptors); index++)
			close(descriptors[index]);
	}
	for (index = 0; index < 300; index++) {
		extra = ioctl(kvm->sys_fd, KVM_CREATE_VM, CVM_VM_TYPE);
		if (extra < 0)
			die("FAIL: metadata recycling iteration=%u errno=%d", index, errno);
		close(extra);
	}
	pr_info("CHECK: CVM capacity=32 exhaustion=ENOMEM refill/rollback/retry=passed metadata_cycles=300");
}

bool cove_io__resource_test(struct kvm *kvm)
{
	const char *enabled = getenv("COVE_IO_TEST_RESOURCES");
	struct kvm_cove_io_tdi request, allocated[KVM_COVE_IO_MAX_TDIS];
	unsigned int existing = 0, count = 0, index;
	u64 cursor = 0;
	int foreign;

	if (!enabled || !strcmp(enabled, "0"))
		return false;
	if (!kvm->cfg.cmode)
		die("FAIL: resources test requires --cvm");
	test_vm_capacity(kvm);
	for (;;) {
		request = (struct kvm_cove_io_tdi) {
			.op = KVM_COVE_IO_TDI_ENUM_OWNED, .tdi_id = cursor,
		};
		test_op(kvm->vm_fd, &request, true, "enumeration");
		if (!request.tdi_id)
			break;
		cursor = request.tdi_id;
		existing++;
	}
	while (existing + count < KVM_COVE_IO_MAX_TDIS) {
		request = (struct kvm_cove_io_tdi) {
			.op = KVM_COVE_IO_TDI_REGISTER,
			.flags = KVM_COVE_IO_TDI_F_AUTO_ID,
		};
		test_op(kvm->vm_fd, &request, true, "TDI fill");
		request.op = KVM_COVE_IO_TDI_BIND;
		test_op(kvm->vm_fd, &request, true, "TDI bind");
		allocated[count++] = request;
	}
	if (!count)
		die("FAIL: no spare TDI for capacity tests");
	request = (struct kvm_cove_io_tdi) {
		.op = KVM_COVE_IO_TDI_REGISTER,
		.flags = KVM_COVE_IO_TDI_F_AUTO_ID,
	};
	test_op(kvm->vm_fd, &request, false, "TDI exhaustion");
	foreign = ioctl(kvm->sys_fd, KVM_CREATE_VM, CVM_VM_TYPE);
	if (foreign < 0)
		die("FAIL: foreign CVM creation errno=%d", errno);
	request = allocated[0];
	request.op = KVM_COVE_IO_TDI_STOP;
	test_op(foreign, &request, false, "foreign STOP");
	close(foreign);
	test_irq_capacity(kvm->vm_fd, allocated[0].tdi_id);
	for (index = 0; index < count; index++) {
		request = allocated[index];
		request.op = KVM_COVE_IO_TDI_UNBIND;
		test_op(kvm->vm_fd, &request, true, "TDI unbind");
		request.op = KVM_COVE_IO_TDI_UNREGISTER;
		test_op(kvm->vm_fd, &request, true, "TDI free");
	}
	request = (struct kvm_cove_io_tdi) {
		.op = KVM_COVE_IO_TDI_REGISTER,
		.flags = KVM_COVE_IO_TDI_F_AUTO_ID,
	};
	test_op(kvm->vm_fd, &request, true, "TDI retry");
	request.op = KVM_COVE_IO_TDI_UNREGISTER;
	test_op(kvm->vm_fd, &request, true, "TDI retry cleanup");
	pr_info("CHECK: TDI capacity=16 exhaustion/retry=passed foreign STOP=denied");
	pr_info("PASS: COVE-IO resource selftest");
	return true;
}
#else
bool cove_io__resource_test(struct kvm *kvm)
{
	return false;
}
#endif
