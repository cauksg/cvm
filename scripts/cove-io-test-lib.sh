#!/bin/sh

cove_io_check_guest_log()
{
	local log="$1" test="$2" tdi_id
	if grep -Eq 'FAIL:|autorun: FAIL|selftest: FAIL|(^|[[:space:]])(WARNING:|BUG:)|Kernel panic|Unhandled trap|use-after-free|COVE-IO .*failed|COVE-IO stale teardown generation accepted|COVE-IO owner enumeration (did not advance|exceeded)' "$log"; then
		echo "FAIL: failure marker in $log" >&2
		tail -100 "$log" >&2
		return 1
	fi
	grep -q "COVE-IO guest autorun: PASS $test" "$log" || {
		echo "FAIL: missing Guest completion in $log" >&2
		return 1
	}
	if grep -q 'cvm_cpu_state_test=1' "$log"; then
		grep -q 'CVM CPU state selftest: PASS GPR/FP/FCSR/SSCRATCH/STVEC/SATP across 16 Host exits' "$log" || {
			echo "FAIL: missing private CPU state probe in $log" >&2
			return 1
		}
	fi
	grep -q 'COVE-IO TDI [0-9][0-9]* bound' "$log" || {
		echo "FAIL: no bound interfaces in $log" >&2
		return 1
	}
	for tdi_id in $(sed -n 's/.*COVE-IO TDI \([0-9][0-9]*\) bound.*/\1/p' "$log" | sort -u); do
		grep -Eq "COVE-IO teardown verified: tdi=$tdi_id generation=[0-9]+->[0-9]+ state=free runtime=revoked" "$log" || {
			echo "FAIL: missing teardown for TDI $tdi_id in $log" >&2
			return 1
		}
	done
}
