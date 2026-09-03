// SPDX-License-Identifier: GPL-2.0
/* Early CoVE-IO guest acceptance for the xs-cvm simulator. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/pgtable.h>
#include <asm/sbi.h>

static bool cove_io_covg_selftest;
static struct sbi_cove_io_interface_report cove_io_report __initdata;
static struct sbi_cove_io_interface_report cove_io_running_report __initdata;
#define COVE_IO_SIM_MAX_INTERFACES	16
#define COVE_IO_SIM_TEST_MMIO_GPA	0x2ff00000ULL
#define COVE_IO_SIM_TEST_MMIO_SIZE	0x1000ULL

static int __init cove_io_covg_selftest_setup(char *arg)
{
	return kstrtobool(arg, &cove_io_covg_selftest);
}
early_param("cove_io_covg_selftest", cove_io_covg_selftest_setup);

static bool __init cove_io_covg_expect(unsigned long id, const char *name,
				       struct sbiret ret, long expected)
{
	if (ret.error == expected)
		return true;

	pr_err("CoVE-IO COVG selftest: FAIL TDI %lu %s error=%ld expected=%ld\n",
	       id, name, ret.error, expected);
	return false;
}

static unsigned int __init
cove_io_covg_enumerate(unsigned long *ids, unsigned int max,
		       unsigned int *failures)
{
	struct sbiret ret;
	unsigned long cursor = 0;
	unsigned int count;

	for (count = 0; count < max; count++) {
		ret = sbi_ecall(SBI_EXT_COVG,
				SBI_EXT_COVG_SIM_GET_INTERFACE_ID,
				cursor, 0, 0, 0, 0, 0);
		if (ret.error == SBI_ERR_NOT_SUPPORTED && !cursor)
			return 0;
		if (ret.error || !ret.value)
			break;
		if ((unsigned long)ret.value <= cursor) {
			pr_err("CoVE-IO COVG selftest: FAIL enumeration did not advance cursor=%lu next=%ld\n",
			       cursor, ret.value);
			(*failures)++;
			break;
		}
		ids[count] = ret.value;
		cursor = ret.value;
	}

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_SIM_GET_INTERFACE_ID,
			cursor, 0, 0, 0, 0, 0);
	if (ret.error || ret.value) {
		pr_err("CoVE-IO COVG selftest: FAIL enumeration end error=%ld value=%ld\n",
		       ret.error, ret.value);
		(*failures)++;
	}

	ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_SIM_GET_INTERFACE_ID,
			~0UL, 0, 0, 0, 0, 0);
	if (!cove_io_covg_expect(~0UL, "enumerate-invalid-cursor", ret,
				 SBI_ERR_INVALID_PARAM))
		(*failures)++;

	return count;
}

void __init riscv_cove_io_guest_accept(void)
{
	struct sbiret probe;
	unsigned int accepted = 0;
	unsigned int selftest_candidates = 0;
	unsigned int selftest_failures = 0;
	unsigned long ids[COVE_IO_SIM_MAX_INTERFACES] = { 0 };
	unsigned int nr_ids;
	unsigned int index;
	unsigned long lifecycle_stop_id = 0;

	probe = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_PROBE_EXT,
			 SBI_EXT_COVG, 0, 0, 0, 0, 0);
	if (probe.error || !probe.value)
		return;

	nr_ids = cove_io_covg_enumerate(ids, ARRAY_SIZE(ids),
					&selftest_failures);
	if (!nr_ids) {
		/* Compatibility fallback for monitors predating the simulator ABI. */
		for (index = 0; index < ARRAY_SIZE(ids); index++)
			ids[index] = index + 1;
		nr_ids = ARRAY_SIZE(ids);
		if (cove_io_covg_selftest) {
			pr_err("CoVE-IO COVG selftest: FAIL simulator enumeration unavailable\n");
			selftest_failures++;
		}
	} else {
		pr_info("CoVE-IO: enumerated %u owner-scoped interfaces\n", nr_ids);
	}

	for (index = 0; index < nr_ids; index++) {
		struct sbiret state;
		struct sbiret link;
		struct sbiret report_ret;
		struct sbiret map_ret;
		struct sbiret ret;
		phys_addr_t report_pa;
		unsigned long id = ids[index];

		state = sbi_ecall(SBI_EXT_COVG,
				  SBI_EXT_COVG_GET_INTERFACE_STATE,
				  id, 0, 0, 0, 0, 0);
		if (state.error || state.value != SBI_COVG_INTERFACE_LOCKED)
			continue;
		if (cove_io_covg_selftest)
			selftest_candidates++;

		link = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_GET_DEVICE_LINK,
				 id, 0, 0, 0, 0, 0);
		if (link.error ||
		    !(link.value & SBI_COVE_IO_LINK_F_UP) ||
		    !(link.value & SBI_COVE_IO_LINK_F_SIMULATED)) {
			pr_warn("CoVE-IO: TDI %lu has no usable simulated link (error=%ld)\n",
				id, link.error);
			continue;
		}
		if (cove_io_covg_selftest &&
		    (!(link.value & SBI_COVE_IO_LINK_F_NO_SPDM) ||
		     !(link.value & SBI_COVE_IO_LINK_F_NO_IDE))) {
			pr_err("CoVE-IO COVG selftest: FAIL TDI %lu link security boundary=0x%lx\n",
			       id, link.value);
			selftest_failures++;
		}

		memset(&cove_io_report, 0, sizeof(cove_io_report));
		report_pa = __pa_symbol(&cove_io_report);
		if (cove_io_covg_selftest) {
			struct sbiret test_ret;

			test_ret = sbi_ecall(SBI_EXT_COVG,
					     SBI_EXT_COVG_GET_INTERFACE_REPORT,
					     id, 0, 0, 0, 0, 0);
			if (!cove_io_covg_expect(id, "report-size-query",
						  test_ret, 0) ||
			    test_ret.value != sizeof(cove_io_report)) {
				pr_err("CoVE-IO COVG selftest: FAIL TDI %lu report size=%ld expected=%zu\n",
				       id, test_ret.value, sizeof(cove_io_report));
				selftest_failures++;
			}

			test_ret = sbi_ecall(SBI_EXT_COVG,
					     SBI_EXT_COVG_GET_INTERFACE_REPORT,
					     id, report_pa,
					     sizeof(cove_io_report) - 8,
					     0, 0, 0);
			if (!cove_io_covg_expect(id, "report-short-buffer",
						  test_ret,
						  SBI_ERR_INVALID_PARAM))
				selftest_failures++;

			test_ret = sbi_ecall(SBI_EXT_COVG,
					     SBI_EXT_COVG_GET_INTERFACE_REPORT,
					     id, PAGE_SIZE,
					     sizeof(cove_io_report),
					     0, 0, 0);
			if (!cove_io_covg_expect(id, "report-unmapped-gpa",
						  test_ret,
						  SBI_ERR_INVALID_ADDRESS))
				selftest_failures++;

			test_ret = sbi_ecall(SBI_EXT_COVG,
					     SBI_EXT_COVG_GET_CONNECTION_TRANSCRIPT,
					     id, 0, 0, 0, 0, 0);
			if (!cove_io_covg_expect(id, "transcript-unsupported",
						  test_ret,
						  SBI_ERR_NOT_SUPPORTED))
				selftest_failures++;

			test_ret = sbi_ecall(SBI_EXT_COVG,
					     SBI_EXT_COVG_GET_DEVICE_MEASUREMENTS,
					     id, 0, 0, 0, 0, 0);
			if (!cove_io_covg_expect(id, "measurements-unsupported",
						  test_ret,
						  SBI_ERR_NOT_SUPPORTED))
				selftest_failures++;
		}
		report_ret = sbi_ecall(SBI_EXT_COVG,
					SBI_EXT_COVG_GET_INTERFACE_REPORT,
					id, report_pa,
					sizeof(cove_io_report), 0, 0, 0);
		if (report_ret.error ||
		    cove_io_report.magic != SBI_COVE_IO_INTERFACE_REPORT_MAGIC ||
		    cove_io_report.version != SBI_COVE_IO_INTERFACE_REPORT_VERSION ||
		    cove_io_report.tdi_id != id || !cove_io_report.generation ||
		    cove_io_report.state != SBI_COVG_INTERFACE_LOCKED) {
			pr_warn("CoVE-IO: TDI %lu report validation failed error=%ld "
				"magic=0x%llx version=%llu state=%llu\n",
				id, report_ret.error,
				(unsigned long long)cove_io_report.magic,
				(unsigned long long)cove_io_report.version,
				(unsigned long long)cove_io_report.state);
			continue;
		}
		if (cove_io_report.mmio_gpa == COVE_IO_SIM_TEST_MMIO_GPA &&
		    cove_io_report.mmio_size == COVE_IO_SIM_TEST_MMIO_SIZE)
			lifecycle_stop_id = id;
		if (cove_io_covg_selftest && cove_io_report.mmio_map_generation) {
			pr_err("CoVE-IO COVG selftest: FAIL TDI %lu mapped before guest acceptance generation=%llu\n",
			       id,
			       (unsigned long long)
				       cove_io_report.mmio_map_generation);
			selftest_failures++;
		}

		if (cove_io_report.mmio_size) {
			if (cove_io_covg_selftest) {
				struct sbiret test_ret;

				test_ret = sbi_ecall(SBI_EXT_COVG,
						     SBI_EXT_COVG_START_INTERFACE,
						     id, 0, 0, 0, 0, 0);
				if (!cove_io_covg_expect(id, "start-before-map",
							  test_ret,
							  SBI_ERR_INVALID_STATE))
					selftest_failures++;

				test_ret = sbi_ecall(SBI_EXT_COVG,
						     SBI_EXT_COVG_MAP_INTERFACE_MMIO,
						     id,
						     cove_io_report.mmio_gpa + PAGE_SIZE,
						     cove_io_report.mmio_size,
						     cove_io_report.generation, 0, 0);
				if (!cove_io_covg_expect(id, "map-wrong-gpa",
							  test_ret,
							  SBI_ERR_INVALID_PARAM))
					selftest_failures++;

				test_ret = sbi_ecall(SBI_EXT_COVG,
						     SBI_EXT_COVG_MAP_INTERFACE_MMIO,
						     id, cove_io_report.mmio_gpa,
						     cove_io_report.mmio_size - 1,
						     cove_io_report.generation,
						     0, 0);
				if (!cove_io_covg_expect(id, "map-wrong-size",
							  test_ret,
							  SBI_ERR_INVALID_PARAM))
					selftest_failures++;

				test_ret = sbi_ecall(SBI_EXT_COVG,
						     SBI_EXT_COVG_MAP_INTERFACE_MMIO,
						     id, cove_io_report.mmio_gpa,
						     cove_io_report.mmio_size,
						     cove_io_report.generation ^ 1,
						     0, 0);
				if (!cove_io_covg_expect(id, "map-stale-generation",
							  test_ret,
							  SBI_ERR_INVALID_PARAM))
					selftest_failures++;
			}
			map_ret = sbi_ecall(SBI_EXT_COVG,
						SBI_EXT_COVG_MAP_INTERFACE_MMIO,
						id, cove_io_report.mmio_gpa,
						cove_io_report.mmio_size,
						cove_io_report.generation, 0, 0);
			if (map_ret.error) {
				pr_warn("CoVE-IO: TDI %lu MMIO map validation failed error=%ld\n",
					id, map_ret.error);
				continue;
			}
			if (cove_io_covg_selftest) {
				map_ret = sbi_ecall(SBI_EXT_COVG,
							SBI_EXT_COVG_MAP_INTERFACE_MMIO,
							id, cove_io_report.mmio_gpa,
							cove_io_report.mmio_size,
							cove_io_report.generation,
							0, 0);
				if (!cove_io_covg_expect(id, "map-repeat",
							  map_ret, 0))
					selftest_failures++;
			}
		} else if (cove_io_covg_selftest) {
			map_ret = sbi_ecall(SBI_EXT_COVG,
						SBI_EXT_COVG_MAP_INTERFACE_MMIO,
						id, 0, 0,
						cove_io_report.generation,
						0, 0);
			if (!cove_io_covg_expect(id, "map-empty-interface",
						  map_ret, SBI_ERR_INVALID_PARAM))
				selftest_failures++;
		}
		ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_START_INTERFACE,
				id, 0, 0, 0, 0, 0);
		if (!ret.error || ret.error == SBI_ERR_ALREADY_STARTED) {
			if (cove_io_covg_selftest) {
				state = sbi_ecall(SBI_EXT_COVG,
						  SBI_EXT_COVG_GET_INTERFACE_STATE,
						  id, 0, 0, 0, 0, 0);
				if (state.error ||
				    state.value != SBI_COVG_INTERFACE_RUNNING) {
					pr_err("CoVE-IO COVG selftest: FAIL TDI %lu running state error=%ld value=%ld\n",
					       id, state.error, state.value);
					selftest_failures++;
				}

				memset(&cove_io_running_report, 0,
				       sizeof(cove_io_running_report));
				report_ret = sbi_ecall(
					SBI_EXT_COVG,
					SBI_EXT_COVG_GET_INTERFACE_REPORT,
					id, __pa_symbol(&cove_io_running_report),
					sizeof(cove_io_running_report), 0, 0, 0);
				if (report_ret.error ||
				    cove_io_running_report.state !=
					    SBI_COVG_INTERFACE_RUNNING ||
				    (cove_io_running_report.mmio_size &&
				     cove_io_running_report.mmio_map_generation !=
					     cove_io_running_report.generation)) {
					pr_err("CoVE-IO COVG selftest: FAIL TDI %lu running report error=%ld map_generation=%llu generation=%llu\n",
					       id, report_ret.error,
					       (unsigned long long)
						       cove_io_running_report.mmio_map_generation,
					       (unsigned long long)
						       cove_io_running_report.generation);
					selftest_failures++;
				}

				ret = sbi_ecall(SBI_EXT_COVG,
						SBI_EXT_COVG_START_INTERFACE,
						id, 0, 0, 0, 0, 0);
				if (!cove_io_covg_expect(id, "start-repeat", ret,
							  SBI_ERR_ALREADY_STARTED))
					selftest_failures++;
			}
			pr_info("CoVE-IO: TDI %lu accepted link=0x%llx "
				"mmio=0x%llx+0x%llx dma=0x%llx+0x%llx\n",
				id, (unsigned long long)cove_io_report.link_flags,
				(unsigned long long)cove_io_report.mmio_gpa,
				(unsigned long long)cove_io_report.mmio_size,
				(unsigned long long)cove_io_report.dma_gpa,
				(unsigned long long)cove_io_report.dma_size);
			accepted++;
		}
	}

	if (cove_io_covg_selftest && lifecycle_stop_id) {
		struct sbiret stop;
		struct sbiret state;
		struct sbiret ret;
		unsigned long stop_id = lifecycle_stop_id;
		u64 generation;

		memset(&cove_io_report, 0, sizeof(cove_io_report));
		ret = sbi_ecall(SBI_EXT_COVG,
				SBI_EXT_COVG_GET_INTERFACE_REPORT, stop_id,
				__pa_symbol(&cove_io_report), sizeof(cove_io_report),
				0, 0, 0);
		generation = cove_io_report.generation;
		if (ret.error) {
			selftest_failures++;
		} else {
			stop = sbi_ecall(SBI_EXT_COVG,
					 SBI_EXT_COVG_STOP_INTERFACE,
					 stop_id, 0, 0, 0, 0, 0);
			if (!cove_io_covg_expect(stop_id, "stop", stop, 0))
				selftest_failures++;

			state = sbi_ecall(SBI_EXT_COVG,
					  SBI_EXT_COVG_GET_INTERFACE_STATE,
					  stop_id, 0, 0, 0, 0, 0);
			if (state.error || state.value != SBI_COVG_INTERFACE_ERROR) {
				pr_err("CoVE-IO COVG selftest: FAIL TDI %lu stop state error=%ld value=%ld\n",
				       stop_id, state.error, state.value);
				selftest_failures++;
			}

			memset(&cove_io_running_report, 0,
			       sizeof(cove_io_running_report));
			ret = sbi_ecall(SBI_EXT_COVG,
					SBI_EXT_COVG_GET_INTERFACE_REPORT, stop_id,
					__pa_symbol(&cove_io_running_report),
					sizeof(cove_io_running_report), 0, 0, 0);
			if (ret.error ||
			    cove_io_running_report.generation != generation ||
			    cove_io_running_report.mmio_map_generation) {
				pr_err("CoVE-IO COVG selftest: FAIL TDI %lu stop report error=%ld generation=%llu map_generation=%llu\n",
				       stop_id, ret.error,
				       (unsigned long long)cove_io_running_report.generation,
				       (unsigned long long)cove_io_running_report.mmio_map_generation);
				selftest_failures++;
			}

			ret = sbi_ecall(SBI_EXT_COVG,
					SBI_EXT_COVG_START_INTERFACE,
					stop_id, 0, 0, 0, 0, 0);
			if (!cove_io_covg_expect(stop_id, "restart-while-stopping", ret,
						 SBI_ERR_INVALID_STATE))
				selftest_failures++;
			stop = sbi_ecall(SBI_EXT_COVG,
					 SBI_EXT_COVG_STOP_INTERFACE,
					 stop_id, 0, 0, 0, 0, 0);
			if (!cove_io_covg_expect(stop_id, "stop-repeat", stop,
						 SBI_ERR_ALREADY_STOPPED))
				selftest_failures++;
			pr_info("CoVE-IO: TDI %lu stop transaction revoked runtime access\n",
				stop_id);
		}
	} else if (cove_io_covg_selftest) {
		pr_err("CoVE-IO COVG selftest: FAIL lifecycle test interface missing\n");
		selftest_failures++;
	}

	if (accepted)
		pr_info("CoVE-IO: guest accepted %u device interfaces\n", accepted);
	if (cove_io_covg_selftest) {
		if (!selftest_candidates || selftest_failures ||
		    accepted != selftest_candidates) {
			pr_err("CoVE-IO COVG selftest: FAIL candidates=%u failures=%u accepted=%u\n",
			       selftest_candidates, selftest_failures, accepted);
		} else {
			pr_info("CoVE-IO COVG selftest: PASS interfaces=%u\n",
				selftest_candidates);
		}
	}
}
