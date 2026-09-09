# dhcpd — DHCP-клиент CactOS

Userspace-демон, который держит на сетевой карте адрес по DHCP
(аналог `dhclient`/`dhcpcd`). Ядро CactOS DHCP-клиент **не** выполняет —
оно лишь применяет конфиг, который присылает пользовательский демон.

## Принцип работы

1. Ждёт появления карты (`CACT_NETCTL_NETCFG_GET` -> `link_up`).
2. Открывает UDP-сокет `0.0.0.0:68`.
3. `DISCOVER` -> `OFFER` -> `REQUEST` -> `ACK` (broadcast `255.255.255.255:67`).
4. Полученные `ip / mask / gw / dns` применяет в ядро через
   `/dev/net` (`CACT_NETCTL_NETCFG`).
5. Спит до `T1` и продлевает аренду (unicast серверу), затем до `T2`
   (broadcast/rebind). При неудаче аренда перезапускается с `DISCOVER`.

## Запуск

```
/sbin/dhcpd            # клиент на eth0
/sbin/dhcpd -i eth0    # явно указать интерфейс (карта в CactOS одна)
```

Демону нужен root (ioctl `CACT_NETCTL_NETCFG` разрешён только root).

## Сборка

```
make CACTLIB=../CactLibc-x86_32
make install LR_SBIN=../LocalRepoCactOS-x86_32/lib/sbin
```

## Зависимости

* CactLibc (`../CactLibc-x86_32`) — `clibc.so` и `build/pic/start.o`
* Ядро с ABI `CACT_NETCTL_NETCFG` / `CACT_NETCTL_NETCFG_GET` (ioctl_abi.h)

Совместно с `networkd` демон обычно не нужен как отдельный процесс: `networkd`
сам умеет статическую настройку и запуск `dhcpd`, если конфиг велит `dhcp=yes`.
