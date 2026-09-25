# dhcpd — CactOS DHCP client

A userspace daemon that keeps a DHCP lease on the NIC (like
`dhclient`/`dhcpcd`). The CactOS kernel does **not** run a DHCP client — it only
applies the configuration the userspace daemon sends it.

## How it works

1. Wait for the NIC to appear (`CACT_NETCTL_NETCFG_GET` -> `link_up`).
2. Open a UDP socket on `0.0.0.0:68`.
3. `DISCOVER` -> `OFFER` -> `REQUEST` -> `ACK` (broadcast to `255.255.255.255:67`).
4. Apply the received `ip / mask / gw / dns` to the kernel through
   `/dev/net` (`CACT_NETCTL_NETCFG`).
5. Sleep until `T1` and renew the lease (unicast to the server), then until `T2`
   (broadcast/rebind). On failure, restart from `DISCOVER`.

## Running

```
/sbin/dhcpd            # client on eth0
/sbin/dhcpd -i eth0    # pick the interface explicitly (CactOS has one NIC)
```

The daemon needs root (the `CACT_NETCTL_NETCFG` ioctl is root-only).

## Building

```
meson setup build-meson --cross-file cross/i686-cact-clang.ini -Dcactlib=../CactLibc-x86_32
ninja -C build-meson            # build-meson/dhcpd
ninja -C build-meson stage      # copy into ../LocalRepoCactOS-x86_32/lib/sbin (-Dlr_sbin)
```

## Dependencies

* CactLibc (`../CactLibc-x86_32`) — `clibc.so` and `build-meson/start.o`
* A kernel with the `CACT_NETCTL_NETCFG` / `CACT_NETCTL_NETCFG_GET` ABI (ioctl_abi.h)

Together with `networkd` the daemon is usually not needed as a separate process:
`networkd` handles static configuration itself and can start `dhcpd` when the
config says `dhcp=yes`.
