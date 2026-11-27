#!/bin/sh
# Usage: fleettest.sh DIR
# DIR holds bedrock-fleet and bzImage-x86_64 (make fleet). Starts the fleet server,
# boots the x86_64 kernel in QEMU with user networking (the host is 10.0.2.2),
# bedrock.report and bedrock.fleet=..., and passes when the server stored a record.
set -eu
dir=$1
port=18080
timeout=${FLEETTEST_TIMEOUT:-900}
t=$(mktemp -d)
trap 'kill ${spid:-} ${qpid:-} 2>/dev/null || true; rm -rf "$t"' EXIT

"$dir/bedrock-fleet" -listen "127.0.0.1:$port" -data "$t/data" -images "$dir" >"$t/server.log" 2>&1 &
spid=$!
: >"$t/serial.log"
qemu-system-x86_64 -m 1024 -kernel "$dir/bzImage-x86_64" -no-reboot -display none -serial file:"$t/serial.log" \
	-append "quiet loglevel=0 console=ttyS0,115200 bedrock.report bedrock.fleet=http://10.0.2.2:$port" \
	-netdev user,id=n0 -device e1000,netdev=n0 &
qpid=$!

i=0
while [ $i -lt "$timeout" ] && kill -0 $qpid 2>/dev/null && ! ls "$t"/data/*.json >/dev/null 2>&1; do
	sleep 1
	i=$((i + 1))
done
sleep 2 # let the client print its last line
kill $qpid 2>/dev/null || true

tail -n 8 "$t/serial.log"
cat "$t/server.log"
ls -l "$t/data"
set -- "$t"/data/*.json
[ -f "$1" ] # the server stored a record
grep -q '"fleet.url":"http://10.0.2.2:'"$port"'"' "$1"
grep -q 'Sent to fleet server' "$t/serial.log"
wget -qO- "http://127.0.0.1:$port/inventory.csv" | tee "$t/inv.csv"
[ "$(wc -l <"$t/inv.csv")" -eq 2 ]
wget -qO- "http://127.0.0.1:$port/boot/boot.ipxe" | grep -q 'bedrock.fleet='
echo "fleettest: ok"
