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
5. Sleep until `T1` and renew the lease (unicast to the server, then
   broadcast/rebind if it does not answer). On failure, wait out the lease and
   restart from `DISCOVER`.

## Running

```
/usr/sbin/dhcpd            # client on eth0
/usr/sbin/dhcpd -i eth0    # pick the interface explicitly (CactOS has one NIC)
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

`dhcpd` is the DHCP client; for static addressing use the `ip` tool, which writes
the same `CACT_NETCTL_NETCFG` configuration. `netd` (`Cact-netd-x86_32`) does
**not** configure the interface — it only watches link/IP/gateway/DNS changes and
logs them. To start `dhcpd` at boot, add it to the `services` list in
`/etc/cgoct.conf`.
