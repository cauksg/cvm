#!/bin/bash
set -euo pipefail

TOP_DIR=$(cd "$(dirname "$0")/.." && pwd)
test_name=${1:-covg}
log=${TEST_LOG:-$(mktemp /tmp/xs-cvm-test.XXXXXX.log)}
overall_timeout=${QEMU_TIMEOUT:-300}
case "$overall_timeout" in ''|*[!0-9]*|0) echo 'QEMU_TIMEOUT must be positive' >&2; exit 2 ;; esac
printf 'Test=%s log=%s\n' "$test_name" "$log"
if ! timeout -k 5s "$overall_timeout" env HOST_COVE_IO_TEST="$test_name" \
	HOST_COVE_IO_TEST_POWER_OFF=1 bash "$TOP_DIR/boot-host-os.sh" >"$log" 2>&1; then
	tail -80 "$log" >&2
	echo 'FAIL: QEMU failed or timed out' >&2
	exit 1
fi
if grep -Eq 'FAIL:|autorun: FAIL|selftest: FAIL|Kernel panic|(^|[[:space:]])(WARNING:|BUG:)' "$log" ||
   ! grep -Fq "COVE-IO host autorun: PASS $test_name" "$log"; then
	tail -80 "$log" >&2
	echo 'FAIL: missing Host PASS or failure marker present' >&2
	exit 1
fi
if [ "$test_name" = edu-direct ] || [ "$test_name" = nic-direct ] || [ "$test_name" = multi-direct ] || [ "$test_name" = multicvm-direct ] || [ "$test_name" = multicvm-nic-pair ] || [ "$test_name" = multicvm-smp-nic-pair ]; then
	for marker in 'direct MMIO: .*installed .*fence=0' \
		'direct MMIO: .*revoked .*fence=0' \
		'COVE-IO IOMMU STOP:'; do
		grep -Eq "$marker" "$log" || { echo "FAIL: missing $marker" >&2; exit 1; }
	done
fi
if [ "$test_name" = edu-direct ] || [ "$test_name" = nic-direct ] || [ "$test_name" = multi-direct ] || [ "$test_name" = multicvm-direct ] || [ "$test_name" = multicvm-nic-pair ] || [ "$test_name" = multicvm-smp-nic-pair ]; then
	grep -Eq 'COVE-IO IMSIC direct remap (installed|refreshed): .*mode=BASIC' "$log" || {
		echo 'FAIL: missing IMSIC BASIC direct delivery' >&2
		exit 1
	}
	if grep -Eq 'COVE-IO experimental MRIF remap (installed|refreshed)' "$log"; then
		echo 'FAIL: device used MRIF instead of direct IMSIC' >&2
		exit 1
	fi
fi
if [ "$test_name" = nic-direct ] || [ "$test_name" = multi-direct ] || [ "$test_name" = multicvm-direct ] || [ "$test_name" = multicvm-nic-pair ] || [ "$test_name" = multicvm-smp-nic-pair ]; then
	grep -Fq 'COVE-IO IOMMU STOP: DMA/MSI revoked' "$log" || {
		echo 'FAIL: missing NIC IOMMU revocation' >&2
		exit 1
	}
	if grep -Eq 'cove_io=allow-vfio-map|private DMA preparation failed|MSI interrupt test failed|NETDEV WATCHDOG|dynamic INTx IRQ bind' "$log"; then
		echo 'FAIL: NIC DMA recovery, MSI fallback or unauthorized INTx path' >&2
		exit 1
	fi
fi
if [ "$test_name" = multi-direct ]; then
    [ "$(grep 'direct MMIO: .*installed .*fence=0' "$log" | sort -u | wc -l)" -ge 2 ] || {
        echo 'FAIL: missing two independent direct BAR mappings' >&2; exit 1;
    }
    [ "$(sed -n 's/.*COVE-IO IMSIC direct remap installed: device_id=\([^ ]*\).*/\1/p' "$log" | sort -u | wc -l)" -ge 2 ] || {
        echo 'FAIL: missing two distinct device IMSIC remaps' >&2; exit 1;
    }
    [ "$(sed -n 's/.*COVE-IO IMSIC direct remap installed: .* iid=\([^ ]*\).*/\1/p' "$log" | sort -u | wc -l)" -ge 2 ] || {
        echo 'FAIL: missing distinct IMSIC identities' >&2; exit 1;
    }
    grep -Fq 'COVE-IO multi: NIC DMA/MSI survives independent EDU STOP' "$log" || exit 1
fi
if [ "$test_name" = multicvm-direct ]; then
    grep -Fq 'CVM IMSIC owner: cvm=1 file=1' "$log" || exit 1
    grep -Fq 'CVM IMSIC owner: cvm=2 file=2' "$log" || exit 1
    grep -Fq 'target=0x0000000028001000 mode=BASIC' "$log" || exit 1
    grep -Fq 'target=0x0000000028002000 mode=BASIC' "$log" || exit 1
    grep -Fq 'COVE-IO multi-CVM: B destroyed while A remains alive' "$log" || exit 1
    grep -Fq 'PASS: two live single-hart CVMs independent private DMA IMSIC and teardown' "$log" || exit 1
fi
if [ "$test_name" = multicvm-smp-nic-pair ]; then
    for mapping in '1 1 0' '1 2 1' '2 3 0' '2 4 1'; do
        read -r vm file cpu <<< "$mapping"
        grep -Eq "CVM IMSIC owner: cvm=$vm file=$file .*vcpu=$cpu" "$log" || exit 1
    done
    for rid in 010 018 020 028; do
        case "$rid" in
            010) target=28001000; cpu=0 ;; 018) target=28002000; cpu=1 ;;
            020) target=28003000; cpu=0 ;; 028) target=28004000; cpu=1 ;;
        esac
        grep -Eq "COVE-IO IMSIC direct remap installed: device_id=0x200000000000$rid vcpu=$cpu .*target=0x00000000$target mode=BASIC" "$log" || exit 1
        grep -Fq "COVE-IO IOMMU STOP: DMA/MSI revoked device=0x200000000000$rid" "$log" || exit 1
    done
    [ "$(grep -c 'CVM SMP selftest: PASS cpus=2 private-state/IPI' "$log")" -ge 2 ] || exit 1
    for cpu in 0 1; do
        [ "$(grep -c "COVE-IO NIC: targeted MSI verified .*cpu=$cpu" "$log")" -ge 2 ] || exit 1
    done
    grep -Fq 'COVE-IO multi-CVM: B destroyed while A remains alive' "$log" || exit 1
    grep -Fq 'PASS: two live single-hart two-vCPU CVMs two e1000e each four TDIs secondary MSI and teardown' "$log" || exit 1
    if [[ " ${HOST_APPEND_EXTRA:-} " == *' kvm.cvm_state_attack_test=1 '* ]]; then
        for vm in 1 2; do
            for cpu in 0 1; do
                for policy in allow deny; do
                    grep -Fq "COVI injection selftest: PASS cvm=$vm vcpu=$cpu policy=$policy" "$log" || exit 1
                done
                grep -Fq "COVI device forgery selftest: PASS cvm=$vm vcpu=$cpu " "$log" || exit 1
            done
        done
    fi
fi
if [ "$test_name" = multicvm-nic-pair ]; then
    grep -Fq 'CVM IMSIC owner: cvm=1 file=1' "$log" || exit 1
    grep -Fq 'CVM IMSIC owner: cvm=2 file=2' "$log" || exit 1
    for rid in 010 018 020 028; do
        if [ "$rid" = 010 ] || [ "$rid" = 018 ]; then target=28001000; else target=28002000; fi
        grep -Eq "COVE-IO IMSIC direct remap installed: device_id=0x200000000000$rid .*target=0x00000000$target mode=BASIC" "$log" || {
            echo "FAIL: missing RID $rid to owner file $target" >&2; exit 1;
        }
        grep -Fq "COVE-IO IOMMU STOP: DMA/MSI revoked device=0x200000000000$rid" "$log" || exit 1
    done
    [ "$(sed -n 's/.*COVE-IO vfio-pci TDI \([0-9][0-9]*\) started for .*/\1/p' "$log" | sort -u | wc -l)" -eq 4 ] || exit 1
    grep -Fq 'COVE-IO multi-CVM: B destroyed while A remains alive' "$log" || exit 1
    grep -Fq 'PASS: two live single-hart CVMs two e1000e each four independent TDIs private DMA IMSIC and teardown' "$log" || exit 1
fi
printf 'PASS: %s (%s)\n'  "$test_name" "$log"
