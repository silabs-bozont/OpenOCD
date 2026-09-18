#!/bin/sh
# Flashes an RPS image to the Si917 over the SAMD11 CMSIS-DAP bridge.
#
# The NWP only listens for bootloader commands in the window it opens right
# after power-on, so the device is power-cycled through the reset/POC net and
# then reattached immediately.  Reattaching needs a fresh OpenOCD process to
# replay the SWD switch sequence, hence the two invocations.
#
# usage: flash_si917.sh <image.rps>
#        flash_si917.sh --erase
set -e

usage()
{
	echo "usage: $0 <image.rps>" >&2
	echo "       $0 --erase" >&2
}

mode=flash
case "${1-}" in
	--erase|-e)
		if [ "$#" -ne 1 ]; then
			usage
			exit 1
		fi
		mode=erase
		preselect=0
		boot_command=0xa14d
		boot_response=0xab4d
		boot_timeout_ms=120000
		openocd_command="flash erase_sector 0 0 0"
		;;
	--help|-h)
		usage
		exit 0
		;;
	*)
		rps="${1-}"
		if [ "$#" -ne 1 ] || [ ! -f "$rps" ]; then
			usage
			exit 1
		fi

		# Resolve the image before moving to the OpenOCD tree, so relative paths
		# work from whatever directory the script is called in.
		rps="$(cd "$(dirname "$rps")" && pwd)/$(basename "$rps")"
		preselect=1
		boot_command=0xa134
		boot_response=0xab32
		boot_timeout_ms=1500
		openocd_command="flash write_bank 0 $rps 0"
		;;
esac

cd "$(dirname "$0")"

log=$(mktemp)
status=$(mktemp)
trap 'rm -f "$log" "$status"' EXIT

cycle=0
while [ $cycle -lt 5 ]; do
	echo "power-cycling Si917"
	./src/openocd -s tcl -f board/si917_powercycle.cfg >/dev/null 2>&1
	if [ "$mode" = erase ]; then
		echo "erasing Si917 common flash"
	fi

	i=0
	while [ $i -lt 60 ]; do
		# Stream progress while retaining the log and pipeline exit status for
		# the retry decision below.
		: >"$log"
		{
			set +e
			./src/openocd -s tcl -f tcl/board/si917_flash.cfg -c "
init
# Upload mode may be selected while the M4 runs.  Erase is selected later by
# the flash driver, after halt, because erasing its XIP image makes a running M4
# fault and destabilizes SWD.
if {$preselect && ![catch {set v [read_memory 0x4105003c 32 1]}] &&
	([expr {\$v & 0xffff}] == 0xab11)} {
	write_memory 0x22000004 32 1
	sleep 50
	write_memory 0x22000004 32 0
	write_memory 0x4105003c 32 0
	write_memory 0x41050034 32 $boot_command
	set deadline [expr {[clock milliseconds] + $boot_timeout_ms}]
	while {[clock milliseconds] < \$deadline} {
		set v [read_memory 0x4105003c 32 1]
		if {[expr {\$v & 0xffff}] == $boot_response} { break }
	}
}
halt
$openocd_command
shutdown" 2>&1
			echo $? >"$status"
		} | grep --line-buffered -v -e "Error connecting DP" -e "DAP init failed" \
			-e "Examination failed" -e "^Info : SWCLK/TCK" | tee "$log"

		if [ "$(cat "$status")" -eq 0 ]; then
			echo "restarting Si917"
			if ! ./src/openocd -s tcl -f board/si917_powercycle.cfg \
				>/dev/null 2>&1; then
				if [ "$mode" = erase ]; then
					echo "erased Si917 common flash, but failed to restart it" >&2
				else
					echo "flashed $rps, but failed to restart the Si917" >&2
				fi
				exit 1
			fi
			if [ "$mode" = erase ]; then
				echo "erased Si917 common flash and restarted Si917"
			else
				echo "flashed $rps and restarted Si917"
			fi
			exit 0
		fi

		if grep -q "Examination succeed" "$log"; then
			# A valid installed image can leave the bootloader window before SWD
			# attachment finishes.  Only this pre-transfer failure is safe to
			# retry with another power cycle.
			if grep -q -e "during command selection" \
				-e "NWP is not in its bootloader" "$log"; then
				echo "missed Si917 bootloader window; retrying"
				break
			fi

			echo "flashing failed" >&2
			exit 1
		fi
		i=$((i + 1))
	done

	cycle=$((cycle + 1))
done

echo "could not enter the Si917 bootloader after 5 power cycles" >&2
exit 1
