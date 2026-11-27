#!/bin/sh
# Usage: boottest.sh IMG LOG SHOT.png QEMU-COMMAND...
# Boots a copy of IMG with bedrock.report (and a kernel loglevel that puts the
# mount line on serial) added to every kernel command line, saves a screenshot
# of the boot menu to SHOT.png, and passes when the serial console shows that
# the headless report exited 0.
set -eu
img=$1 log=$2 shot=$3
shift 3
timeout=${BOOTTEST_TIMEOUT:-1200}
t=$(mktemp -d)
trap 'kill ${pid:-} 2>/dev/null || true; rm -rf "$t"' EXIT
export MTOOLS_SKIP_CHECK=1
fat="$t/test.img@@1048576"

cp "$img" "$t/test.img"
for f in boot/syslinux/syslinux.cfg boot/grub/grub.cfg; do
	mcopy -n -i "$fat" "::$f" "$t/cfg"
	sed -i 's/console=tty0/console=tty0 bedrock.report loglevel=7/' "$t/cfg"
	mcopy -o -i "$fat" "$t/cfg" "::$f"
done

mkfifo "$t/mon.in" "$t/mon.out"
: >"$log"
"$@" -drive file="$t/test.img",format=raw,if=ide,index=0 -display none -serial file:"$log" -monitor pipe:"$t/mon" &
pid=$!
exec 3<>"$t/mon.out" 4<>"$t/mon.in" # keep both ends open so the monitor never blocks
i=0 shot_taken=
while [ $i -lt "$timeout" ] && kill -0 $pid 2>/dev/null && ! grep -q 'bedrock --report: exit' "$log"; do
	if [ -z "$shot_taken" ] && grep -q 'Start Bedrock' "$log"; then
		sleep 2 # let the graphical menu finish drawing
		echo "screendump $shot -f png" >&4
		shot_taken=1
	fi
	sleep 1
	i=$((i + 1))
done
kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true

tail -n 40 "$log"
echo "--- files on the stick after the run:"
mdir -b -i "$fat" ::
ls -l "$shot"
grep -q 'bedrock: report partition .* mounted' "$log" # wipe's boot-stick refusal depends on this mount
grep -q 'bedrock --report: exit 0' "$log"
# No usable GPU in QEMU: the GPU stress must end without a FAIL, and on the 64-bit
# system the software renderer must have run the GPU test program itself.
grep -qE '(INCONCLUSIVE|NOT PRESENT) +GPU' "$log"
case "$*" in *qemu-system-x86_64*) grep -q 'software renderer llvmpipe' "$log" ;; esac
