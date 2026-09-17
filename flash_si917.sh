#!/bin/sh
# Flashes an RPS image to the Si917 over the SAMD11 CMSIS-DAP bridge.
#
# The NWP only listens for bootloader commands in the window it opens right
# after power-on, so the device is power-cycled through the reset/POC net and
# then reattached immediately.  Reattaching needs a fresh OpenOCD process to
# replay the SWD switch sequence, hence the two invocations.
#
# usage: flash_si917.sh <image.rps>
set -e

rps="$1"
if [ -z "$rps" ] || [ ! -f "$rps" ]; then
	echo "usage: $0 <image.rps>" >&2
	exit 1
fi

# Resolve the image before moving to the OpenOCD tree, so relative paths work
# from whatever directory the script is called in.
rps="$(cd "$(dirname "$rps")" && pwd)/$(basename "$rps")"
cd "$(dirname "$0")"

log=$(mktemp)
status=$(mktemp)
trap 'rm -f "$log" "$status"' EXIT

cycle=0
while [ $cycle -lt 5 ]; do
	echo "power-cycling Si917"
	./src/openocd -s tcl -f board/si917_powercycle.cfg >/dev/null 2>&1

	i=0
	while [ $i -lt 60 ]; do
		# Stream progress while retaining the log and pipeline exit status for
		# the retry decision below.
		: >"$log"
		{
			set +e
			./src/openocd -s tcl -f tcl/board/si917_flash.cfg -c "
init
# Restart the TA bootloader before selecting M4 upgrade.  The NWP ignores the
# command at board-ready unless TA_RESET is first pulsed from 1 to 0.
if {![catch {set v [read_memory 0x4105003c 32 1]}] &&
	([expr {\$v & 0xffff}] == 0xab11)} {
	write_memory 0x22000004 32 1
	sleep 50
	write_memory 0x22000004 32 0
	write_memory 0x4105003c 32 0
	write_memory 0x41050034 32 0xa134
	set deadline [expr {[clock milliseconds] + 1500}]
	while {[clock milliseconds] < \$deadline} {
		set v [read_memory 0x4105003c 32 1]
		if {[expr {\$v & 0xffff}] == 0xab32} { break }
	}
}
halt
flash write_bank 0 $rps 0
shutdown" 2>&1
			echo $? >"$status"
		} | grep --line-buffered -v -e "Error connecting DP" -e "DAP init failed" \
			-e "Examination failed" -e "^Info : SWCLK/TCK" | tee "$log"

		if [ "$(cat "$status")" -eq 0 ]; then
			echo "restarting Si917"
			if ! ./src/openocd -s tcl -f board/si917_powercycle.cfg \
				>/dev/null 2>&1; then
				echo "flashed $rps, but failed to restart the Si917" >&2
				exit 1
			fi
			echo "flashed $rps and restarted Si917"
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
