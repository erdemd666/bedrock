# Bedrock fleet

Boot many PCs over the network. Each one tests itself and sends its record to one
server, which shows them all on a page and exports an inventory.

```
PC (PXE/iPXE) --boot.ipxe, bzImage--> bedrock-fleet <--POST /api/records-- PC runs the tests
```

The kernel image holds the whole OS, so a PC needs only one file from the server.

**LAN only.** The client talks plain HTTP and the server has no login. Anyone who
can reach it can read and add records. Keep it on a trusted network.

## Build and run

```sh
make fleet        # out/fleet/bedrock-fleet, bzImage-x86_64, bzImage-i686 (needs make img's build volume)
make fleettest    # boots the x86_64 image in QEMU and checks that the server got its record

out/fleet/bedrock-fleet -listen :8080 -data /var/lib/bedrock-fleet -images out/fleet
```

| Flag | Default | |
|---|---|---|
| `-listen` | `:8080` | address and port |
| `-data` | `data` | one JSON file per record, named `<serial>-<time>.json`, stored as received |
| `-images` | `.` | directory with `bzImage-x86_64` and `bzImage-i686` |

Without make: `cd fleet && go build -o bedrock-fleet .` (Go 1.22 or newer, standard library only).

## Pages

| URL | |
|---|---|
| `/` | machines with model, serial, asset tag, result, counts, time; refreshes every 10 s |
| `/m/<id>` | one record: every result and fact, and earlier records of the same machine |
| `/inventory.csv` | the table above as CSV |
| `POST /api/records` | a record as JSON (at most 1 MiB); 201 with `{"id": ...}` |
| `/boot/boot.ipxe` | iPXE script: picks the 64-bit or 32-bit kernel, adds `bedrock.fleet=http://<this server>` |
| `/boot/bzImage-x86_64`, `/boot/bzImage-i686` | the kernels, from `-images` |

The result column is FAIL if any check failed, else INCONCLUSIVE if any could not
measure, else PASS. The dashboard lists the newest record of each machine. Machines
whose firmware serial is a placeholder ("To Be Filled By O.E.M.") are never merged.

## Network setup

Clients need a wired NIC with DHCP and a firmware that can PXE boot. Make the server
address fixed (or give it a DNS name) and use it below as `SERVER`, port 8080.

### A. Existing LAN: dnsmasq as proxy-DHCP (the router keeps handing out addresses)

Run dnsmasq on any machine on the LAN. With `dhcp-range=...,proxy` it only adds the
"boot from here" answer and never leases addresses.

Get the stock iPXE programs from https://boot.ipxe.org (`undionly.kpxe` for BIOS,
`ipxe.efi` for UEFI) into `/srv/tftp`.

```
# /etc/dnsmasq.d/bedrock.conf
port=0
dhcp-range=192.168.1.0,proxy        # your LAN's network address
enable-tftp
tftp-root=/srv/tftp
log-dhcp

dhcp-match=set:ipxe,175             # already running iPXE: give it the script
pxe-service=tag:ipxe,x86PC,"Bedrock",http://SERVER:8080/boot/boot.ipxe
pxe-service=tag:ipxe,BC_EFI,"Bedrock",http://SERVER:8080/boot/boot.ipxe
pxe-service=tag:ipxe,x86-64_EFI,"Bedrock",http://SERVER:8080/boot/boot.ipxe
pxe-service=tag:ipxe,IA32_EFI,"Bedrock",http://SERVER:8080/boot/boot.ipxe

pxe-service=tag:!ipxe,x86PC,"Bedrock",undionly.kpxe   # plain PXE: chainload iPXE first
pxe-service=tag:!ipxe,BC_EFI,"Bedrock",ipxe.efi
pxe-service=tag:!ipxe,x86-64_EFI,"Bedrock",ipxe.efi
pxe-service=tag:!ipxe,IA32_EFI,"Bedrock",ipxe.efi
```

Boot sequence: the PC's PXE ROM asks DHCP, the router answers with an address,
dnsmasq answers with `undionly.kpxe`/`ipxe.efi`; iPXE starts, asks again, and this
time dnsmasq answers with the script URL; iPXE fetches `boot.ipxe`, then the kernel.

### B. Simpler: iPXE directly, no DHCP changes

Write an iPXE USB stick or CD (`ipxe.usb` or `ipxe.iso` from https://boot.ipxe.org),
or press Ctrl-B when iPXE starts, and type:

```
dhcp
chain http://SERVER:8080/boot/boot.ipxe
```

To make that automatic, build iPXE with an embedded script containing those two
lines (`make bin/ipxe.usb EMBED=bedrock.ipxe` in the iPXE sources).

## On the client

Fleet mode starts when the boot line has `bedrock.fleet=URL` (the script adds it) or
`bedrock.cfg` on the stick has a line `fleet_url = http://SERVER:8080`. Then, and
only then, Bedrock brings up the first wired NIC that has a link, gets an address by
DHCP, runs the quick report on the screen, sends the record and prints
"Sent to fleet server" (or why not). Press Enter for the normal menu. The sent
record carries the fact `fleet.url`. Uploads are retried for about 15 seconds.

Wired NICs supported: e1000, e1000e, igb, r8169 (with its firmware), tg3, atl1c, alx,
8139too, virtio on 64-bit; e100, e1000, 8139too, r8169, via-rhine, 3c59x, ne2k-pci on
32-bit. No Wi-Fi, no USB network adapters.

## Not verified

Only QEMU (`make fleettest`: e1000 with user networking, direct kernel boot; the 32-bit kernel was run the same way on a Pentium III CPU model) has run
this. The dnsmasq proxy-DHCP setup, the iPXE script (`${cpuarch}` needs iPXE from
2021 or later; older builds fall back to `${buildarch}`, so a 64-bit PC on BIOS would
then get the 32-bit kernel), UEFI network boot of the kernel, and every NIC driver
other than e1000 are written from the specifications and untested on hardware.
