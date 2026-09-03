#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

CMDLINE="$(cat /proc/cmdline 2>/dev/null || true)"
TEST=""
POWEROFF=1

cmdline_value()
{
	key="$1"

	for token in $CMDLINE; do
		case "$token" in
		"$key="*) printf '%s\n' "${token#*=}"; return 0 ;;
		esac
	done

	return 1
}

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

to_num()
{
	printf '%u\n' "$(( $1 ))"
}

need()
{
	command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

run_covg()
{
	need dmesg
	dmesg > /tmp/cove-io-covg-selftest.dmesg
	if grep -q "CoVE-IO COVG selftest: FAIL" \
		/tmp/cove-io-covg-selftest.dmesg; then
		fail "kernel COVG selftest reported a failure"
	fi
	grep -q "CoVE-IO COVG selftest: PASS" \
		/tmp/cove-io-covg-selftest.dmesg ||
		fail "kernel COVG selftest did not report PASS"
	grep -q "CoVE-IO: guest accepted" \
		/tmp/cove-io-covg-selftest.dmesg ||
		fail "guest did not accept any COVG interfaces"
	grep -q "owner-scoped interfaces" /tmp/cove-io-covg-selftest.dmesg ||
		fail "owner-scoped TDI enumeration was not used"
	grep -q "stop transaction revoked runtime access" \
		/tmp/cove-io-covg-selftest.dmesg ||
		fail "STOP transaction was not exercised"
	echo "COVE-IO guest covg: enumeration/link/report/generation/MMIO-map/STOP checks passed"
}

hex_byte_at()
{
	file="$1"
	offset="$2"

	dd if="$file" bs=1 skip="$offset" count=1 2>/dev/null |
		od -An -tu1 | awk '{ print $1 }'
}

write_bytes()
{
	file="$1"
	offset="$2"
	shift 2

	(
		for byte in "$@"; do
			printf "\\$(printf '%03o' "$byte")"
		done
	) | dd of="$file" bs=1 seek="$offset" conv=notrunc 2>/dev/null
}

write_u16()
{
	file="$1"
	offset="$2"
	value="$3"

	write_bytes "$file" "$offset" \
		"$(( value & 0xff ))" \
		"$(( (value >> 8) & 0xff ))"
}

write_u32()
{
	file="$1"
	offset="$2"
	value="$3"

	write_bytes "$file" "$offset" \
		"$(( value & 0xff ))" \
		"$(( (value >> 8) & 0xff ))" \
		"$(( (value >> 16) & 0xff ))" \
		"$(( (value >> 24) & 0xff ))"
}

write_u64()
{
	file="$1"
	offset="$2"
	value="$3"

	write_u32 "$file" "$offset" "$(( value & 0xffffffff ))"
	write_u32 "$file" "$(( offset + 4 ))" "$(( (value >> 32) & 0xffffffff ))"
}

find_edu_device()
{
	for dev in /sys/bus/pci/devices/*; do
		[ -r "$dev/vendor" ] || continue
		[ -r "$dev/device" ] || continue
		[ "$(cat "$dev/vendor")" = "0x1234" ] || continue
		[ "$(cat "$dev/device")" = "0x11e8" ] || continue
		basename "$dev"
		return 0
	done

	return 1
}

read_u16()
{
	file="$1"
	offset="$2"
	lo="$(hex_byte_at "$file" "$offset")"
	hi="$(hex_byte_at "$file" "$(( offset + 1 ))")"
	printf '%s\n' "$(( lo | (hi << 8) ))"
}

read_u32()
{
	file="$1"
	offset="$2"
	b0="$(hex_byte_at "$file" "$offset")"
	b1="$(hex_byte_at "$file" "$(( offset + 1 ))")"
	b2="$(hex_byte_at "$file" "$(( offset + 2 ))")"
	b3="$(hex_byte_at "$file" "$(( offset + 3 ))")"
	printf '%s\n' "$(( b0 | (b1 << 8) | (b2 << 16) | (b3 << 24) ))"
}

find_msi_cap()
{
	config="$1"
	pos="$(hex_byte_at "$config" 52)"
	limit=48

	while [ "$pos" -ne 0 ] && [ "$limit" -gt 0 ]; do
		cap_id="$(hex_byte_at "$config" "$pos")"
		next="$(hex_byte_at "$config" "$(( pos + 1 ))")"
		if [ "$cap_id" -eq 5 ]; then
			printf '%s\n' "$pos"
			return 0
		fi
		pos="$next"
		limit="$(( limit - 1 ))"
	done

	return 1
}

find_ext_cap()
{
	config="$1"
	wanted="$2"
	pos=256
	limit=64

	while [ "$pos" -ne 0 ] && [ "$limit" -gt 0 ]; do
		hdr="$(read_u32 "$config" "$pos")"
		cap_id="$(( hdr & 0xffff ))"
		next="$(( (hdr >> 20) & 0xffc ))"
		if [ "$cap_id" -eq "$wanted" ]; then
			printf '%s\n' "$pos"
			return 0
		fi
		pos="$next"
		limit="$(( limit - 1 ))"
	done

	return 1
}

resource_bar_start()
{
	bdf="$1"
	bar="$2"
	awk -v n="$(( bar + 1 ))" 'NR == n { print $1 }' \
		"/sys/bus/pci/devices/$bdf/resource"
}

run_edu_msi()
{
	need awk
	need basename
	need dd
	need od

	bdf="$(find_edu_device)" || fail "QEMU edu PCI device not found"
	config="/sys/bus/pci/devices/$bdf/config"
	[ -w "$config" ] || fail "PCI config is not writable for $bdf"

	msi_cap="$(find_msi_cap "$config")" || fail "MSI capability not found for $bdf"
	mode="$(cmdline_value cove_io_guest_msi_mode || printf '%s\n' "${COVE_IO_GUEST_MSI_MODE:-basic}")"
	addr_lo="$(cmdline_value cove_io_guest_msi_addr_lo || printf '%s\n' "${COVE_IO_GUEST_MSI_ADDR_LO:-0x08000000}")"
	addr_hi="$(cmdline_value cove_io_guest_msi_addr_hi || printf '%s\n' "${COVE_IO_GUEST_MSI_ADDR_HI:-0x0}")"
	data="$(cmdline_value cove_io_guest_msi_data || printf '%s\n' "${COVE_IO_GUEST_MSI_DATA:-0x71}")"
	addr1_lo="$(cmdline_value cove_io_guest_msi_retarget_addr_lo || printf '0x%x\n' "$(( addr_lo + 0x1000 ))")"
	addr1_hi="$(cmdline_value cove_io_guest_msi_retarget_addr_hi || printf '%s\n' "$addr_hi")"
	data1="$(cmdline_value cove_io_guest_msi_retarget_data || printf '0x%x\n' "$(( data + 1 ))")"

	edu_msi_write_message "$config" "$msi_cap" "$addr_lo" "$addr_hi" "$data"
	edu_msi_set_enabled "$config" "$msi_cap" 1
	edu_msi_trigger_irq "$bdf" || true

	if [ "$mode" = "retarget" ]; then
		edu_msi_write_message "$config" "$msi_cap" "$addr1_lo" "$addr1_hi" "$data1"
		edu_msi_set_enabled "$config" "$msi_cap" 1
		edu_msi_trigger_irq "$bdf" || true
		echo "COVE-IO guest edu-msi-retarget: retargeted MSI for $bdf addr=0x$(printf '%x' "$addr1_hi")$(printf '%08x' "$addr1_lo") data=$data1"
	fi

	echo "COVE-IO guest edu-msi: enabled MSI for $bdf cap=0x$(printf '%x' "$msi_cap") addr=0x$(printf '%x' "$addr_hi")$(printf '%08x' "$addr_lo") data=$data"
}

edu_msi_write_message()
{
	config="$1"
	msi_cap="$2"
	addr_lo="$3"
	addr_hi="$4"
	data="$5"
	ctrl="$(read_u16 "$config" "$(( msi_cap + 2 ))")"

	if [ "$(( ctrl & 0x0080 ))" -ne 0 ]; then
		write_u32 "$config" "$(( msi_cap + 4 ))" "$addr_lo"
		write_u32 "$config" "$(( msi_cap + 8 ))" "$addr_hi"
		write_u16 "$config" "$(( msi_cap + 12 ))" "$data"
	else
		write_u32 "$config" "$(( msi_cap + 4 ))" "$addr_lo"
		write_u16 "$config" "$(( msi_cap + 8 ))" "$data"
	fi
}

edu_msi_set_enabled()
{
	config="$1"
	msi_cap="$2"
	enable="$3"
	ctrl="$(read_u16 "$config" "$(( msi_cap + 2 ))")"

	if [ "$enable" = "1" ]; then
		ctrl="$(( ctrl | 0x0001 ))"
	else
		ctrl="$(( ctrl & ~0x0001 ))"
	fi
	write_u16 "$config" "$(( msi_cap + 2 ))" "$ctrl"
}

edu_msi_trigger_irq()
{
	bdf="$1"

	command -v devmem >/dev/null 2>&1 || return 0
	bar0="$(resource_bar_start "$bdf" 0)"
	case "$bar0" in
	0x*) ;;
	*) return 0 ;;
	esac
	devmem "$(( bar0 + 0x60 ))" 32 1 >/dev/null
	devmem "$(( bar0 + 0x64 ))" 32 1 >/dev/null
}

run_edu_pri()
{
	need awk
	need basename
	need dd
	need devmem
	need od

	bdf="$(find_edu_device)" || fail "QEMU edu PCI device not found"
	config="/sys/bus/pci/devices/$bdf/config"
	[ -w "$config" ] || fail "PCI config is not writable for $bdf"

	ats_cap="$(find_ext_cap "$config" 15)" ||
		fail "ATS extended capability not found for $bdf"
	pri_cap="$(find_ext_cap "$config" 19)" ||
		fail "PRI extended capability not found for $bdf"
	pasid_cap="$(find_ext_cap "$config" 27 || true)"

	cmd="$(read_u16 "$config" 4)"
	write_u16 "$config" 4 "$(( cmd | 0x6 ))"

	write_u16 "$config" "$(( ats_cap + 6 ))" "$(( 0x8000 | 0 ))"
	if [ -n "$pasid_cap" ]; then
		write_u16 "$config" "$(( pasid_cap + 6 ))" 1
	fi
	write_u32 "$config" "$(( pri_cap + 12 ))" 32
	write_u16 "$config" "$(( pri_cap + 4 ))" 1

	bar0="$(resource_bar_start "$bdf" 0)"
	[ -n "$bar0" ] || fail "cannot read BAR0 resource for $bdf"
	case "$bar0" in
	0x*) ;;
	*) fail "BAR0 is not mapped for $bdf: $bar0" ;;
	esac

	pri_iova="$(cmdline_value cove_io_guest_pri_iova || printf '0x82d00000\n')"
	pri_bad_iova="$(cmdline_value cove_io_guest_pri_bad_iova || printf '0x83000000\n')"
	mode="$(cmdline_value cove_io_guest_pri_mode || printf '%s\n' "${COVE_IO_GUEST_PRI_MODE:-basic}")"
	count="$(cmdline_value cove_io_guest_pri_count || printf '%s\n' "${COVE_IO_GUEST_PRI_COUNT:-16}")"
	overflow_count="$(cmdline_value cove_io_guest_pri_overflow_count || printf '%s\n' "${COVE_IO_GUEST_PRI_OVERFLOW_COUNT:-1024}")"

	case "$mode" in
	basic)
		edu_pri_one "$bar0" "$pri_iova" 0xffffffff 7 1 0 success
		;;
	stress)
		edu_pri_one "$bar0" "$pri_iova" 0xffffffff 32 "$count" 0 success
		;;
	pasid)
		[ -n "$pasid_cap" ] || fail "PASID extended capability not found for $bdf"
		edu_pri_one "$bar0" "$pri_iova" 1 48 1 0 success
		;;
	deny)
		edu_pri_one "$bar0" "$pri_bad_iova" 0xffffffff 64 1 0 invalid
		;;
	cancel)
		edu_pri_one "$bar0" "$pri_iova" 0xffffffff 96 "$count" 0 cancel
		;;
	stop)
		edu_pri_stop "$bar0"
		;;
	overflow)
		edu_pri_one "$bar0" "$pri_iova" 0xffffffff 128 "$overflow_count" 0 error
		;;
	extended)
		edu_pri_one "$bar0" "$pri_iova" 0xffffffff 32 "$count" 0 success
		[ -n "$pasid_cap" ] || fail "PASID extended capability not found for $bdf"
		edu_pri_one "$bar0" "$pri_iova" 1 48 1 0 success
		edu_pri_one "$bar0" "$pri_iova" 2 49 1 0 success
		edu_pri_one "$bar0" "$pri_iova" 0xffffffff 96 "$count" 0 cancel
		edu_pri_stop "$bar0"
		;;
	*)
		fail "unknown cove_io_guest_pri_mode=$mode"
		;;
	esac

	echo "COVE-IO guest edu-pri: success mode=$mode bdf=$bdf iova=$pri_iova ats=0x$(printf '%x' "$ats_cap") pri=0x$(printf '%x' "$pri_cap") pasid=0x$(printf '%x' "${pasid_cap:-0}")"
}

edu_pri_reg32()
{
	bar0="$1"
	off="$2"

	to_num "$(devmem "$(( bar0 + off ))" 32)"
}

edu_pri_reset()
{
	bar0="$1"

	devmem "$(( bar0 + 0xac ))" 32 0x100 >/dev/null
}

edu_pri_one()
{
	bar0="$1"
	iova="$2"
	pasid="$3"
	prgi="$4"
	count="$5"
	stride="$6"
	expect="$7"
	ctrl="$(( 0x1 | 0x2 | 0x4 | 0x8 | 0x20 ))"

	edu_pri_reset "$bar0"
	if [ "$count" -gt 1 ]; then
		ctrl="$(( ctrl | 0x40 ))"
	fi
	devmem "$(( bar0 + 0xa0 ))" 64 "$iova" >/dev/null
	devmem "$(( bar0 + 0xa8 ))" 32 "$pasid" >/dev/null
	devmem "$(( bar0 + 0xb8 ))" 32 "$prgi" >/dev/null
	devmem "$(( bar0 + 0xd4 ))" 32 "$count" >/dev/null
	devmem "$(( bar0 + 0xd8 ))" 64 "$stride" >/dev/null
	devmem "$(( bar0 + 0xac ))" 32 "$ctrl" >/dev/null

	if [ "$expect" = "cancel" ]; then
		devmem "$(( bar0 + 0xac ))" 32 "$(( 0x1 | 0x80 ))" >/dev/null
	fi

	edu_pri_wait "$bar0" "$expect" "$count"
}

edu_pri_stop()
{
	bar0="$1"

	edu_pri_reset "$bar0"
	devmem "$(( bar0 + 0xac ))" 32 "$(( 0x1 | 0x2 | 0x200 ))" >/dev/null
	status="$(edu_pri_reg32 "$bar0" 0xb0)"
	requested="$(edu_pri_reg32 "$bar0" 0xbc)"
	[ "$status" -eq 7 ] || fail "EDU PRI stop did not report stopped status=$status"
	[ "$requested" -eq 0 ] || fail "EDU PRI stop enqueued requests=$requested"
	echo "COVE-IO guest edu-pri-stop: status=$status requested=$requested"
}

edu_pri_wait()
{
	bar0="$1"
	expect="$2"
	count="$3"
	i=0

	while [ "$i" -lt 60 ]; do
		status="$(edu_pri_reg32 "$bar0" 0xb0)"
		completed="$(edu_pri_reg32 "$bar0" 0xc0)"
		success="$(edu_pri_reg32 "$bar0" 0xc4)"
		invalid="$(edu_pri_reg32 "$bar0" 0xc8)"
		failure="$(edu_pri_reg32 "$bar0" 0xcc)"
		errors="$(edu_pri_reg32 "$bar0" 0xd0)"
		cancelled="$(edu_pri_reg32 "$bar0" 0xe4)"
		dropped="$(edu_pri_reg32 "$bar0" 0xe8)"

		case "$expect" in
		success)
			if [ "$success" -ge "$count" ] && [ "$completed" -ge "$count" ]; then
				echo "COVE-IO guest edu-pri-success: count=$count completed=$completed success=$success"
				return 0
			fi
			;;
		invalid)
			if [ "$invalid" -ge "$count" ] || [ "$status" -eq 3 ]; then
				echo "COVE-IO guest edu-pri-invalid: count=$count completed=$completed invalid=$invalid"
				return 0
			fi
			;;
		cancel)
			if [ "$status" -eq 6 ]; then
				echo "COVE-IO guest edu-pri-cancel: completed=$completed cancelled=$cancelled dropped=$dropped"
				return 0
			fi
			;;
		error)
			if [ "$errors" -gt 0 ] || [ "$status" -eq 5 ]; then
				echo "COVE-IO guest edu-pri-error: count=$count completed=$completed errors=$errors failure=$failure"
				return 0
			fi
			;;
		esac

		if [ "$status" -eq 4 ] || [ "$status" -eq 5 ]; then
			if [ "$expect" != "error" ]; then
				err="$(devmem "$(( bar0 + 0xb4 ))" 32)"
				fail "EDU PRI failed status=$status error=$err expect=$expect completed=$completed"
			fi
		fi
		sleep 1
		i="$(( i + 1 ))"
	done

	err="$(devmem "$(( bar0 + 0xb4 ))" 32)"
	fail "EDU PRI timed out expect=$expect status=$status error=$err completed=$completed success=$success invalid=$invalid errors=$errors"
}

TEST="$(cmdline_value cove_io_guest_test || true)"
[ -n "$TEST" ] || exit 0
POWEROFF="$(cmdline_value cove_io_guest_poweroff || printf '1\n')"

echo "COVE-IO guest autorun: test=$TEST"
case "$TEST" in
covg|covg-abi)
	run_covg
	;;
edu-msi|msi)
	run_edu_msi
	;;
edu-msi-retarget|msi-retarget)
	COVE_IO_GUEST_MSI_MODE=retarget
	run_edu_msi
	;;
edu-pri|pri)
	run_edu_pri
	;;
edu-pri-*|pri-*)
	mode="${TEST#edu-pri-}"
	mode="${mode#pri-}"
	COVE_IO_GUEST_PRI_MODE="$mode"
	run_edu_pri
	;;
*)
	fail "unknown cove_io_guest_test=$TEST"
	;;
esac

echo "COVE-IO guest autorun: PASS $TEST"

if [ "$POWEROFF" != "0" ]; then
	poweroff -f || reboot -f || true
fi
