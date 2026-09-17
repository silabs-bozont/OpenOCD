#!/bin/sh
# Observes the NWP mailbox after a power cycle.
#
# With no argument it only watches, to show how long the bootloader window
# stays open on its own.  With "write" it issues the M4 upgrade command
# (0xa134) as soon as board-ready (0xab11) appears and logs what comes back.
cd "$(dirname "$0")"

mode="$1"
cmd="${2:-0xa134}"

DAP="
swj_newdap si917 cpu -irlen 4 -expected-id 0x2ba01477
dap create si917.dap -chain-position si917.cpu
target create si917.cpu cortex_m -dap si917.dap -defer-examine"

./src/openocd -s tcl -f interface/cmsis-dap.cfg \
	-c "transport select swd; adapter speed 1000; reset_config srst_only srst_push_pull" \
	-f target/swj-dp.tcl -c "$DAP
catch {init}
adapter assert srst
sleep 200
adapter deassert srst
shutdown" >/dev/null 2>&1

i=0
while [ $i -lt 60 ]; do
	out=$(./src/openocd -s tcl -f tcl/board/si917_flash.cfg -c "
init
catch {halt}
set t0 [clock milliseconds]
set seen \"\"
set wrote 0
while {[clock milliseconds] - \$t0 < 3000} {
	if {[catch {set v [read_memory 0x4105003c 32 1]}]} { break }
	set hex [format 0x%04x [expr {\$v & 0xffff}]]
	if {\$hex ne \$seen} {
		echo \"T+[expr {[clock milliseconds] - \$t0}]ms out=\$hex\"
		set seen \$hex
	}
	if {\"$mode\" ne \"watch\" && !\$wrote && \$hex eq \"0xab11\"} {
		if {\"$mode\" eq \"tareset\"} {
			write_memory 0x22000004 32 0
			echo \"T+[expr {[clock milliseconds] - \$t0}]ms TA reset = 0\"
		}
		if {[string match \"tapulse*\" \"$mode\"]} {
			set lvl [string index \"$mode\" 7]
			write_memory 0x22000004 32 \$lvl
			sleep 50
			write_memory 0x22000004 32 [expr {!\$lvl}]
			echo \"T+[expr {[clock milliseconds] - \$t0}]ms TA pulsed \$lvl -> [expr {!\$lvl}]\"
			set wrote 1
			set seen \"\"
			continue
		}
		write_memory 0x4105003c 32 0
		write_memory 0x41050034 32 $cmd
		set wrote 1
		set back [format 0x%08x [read_memory 0x41050034 32 1]]
		echo \"T+[expr {[clock milliseconds] - \$t0}]ms wrote $cmd, IN reads back \$back\"
		set seen \"\"
	}
	if {\$hex eq \"0xab32\"} { echo \"GOT 0xab32 - ready for RPS\"; break }
}
echo \"T+[expr {[clock milliseconds] - \$t0}]ms end, last=\$seen\"
shutdown" 2>&1 | grep -E "T\+|GOT")
	if [ -n "$out" ]; then
		echo "$out"
		exit 0
	fi
	i=$((i + 1))
done

echo "never reattached" >&2
exit 1
