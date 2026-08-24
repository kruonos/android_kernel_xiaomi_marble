# QCA6490 monitor injection tools

Static aarch64 userspace tools and on-device test scripts used for the
QCA6490 monitor/STA management TX experiments on POCO F5 (marble).

## Build

```sh
aarch64-linux-gnu-gcc -static -O2 -o nl80211_probe nl80211_probe.c
aarch64-linux-gnu-gcc -static -O2 -o nl80211_probe_tx nl80211_probe_tx.c
aarch64-linux-gnu-gcc -static -O2 -o nl80211_mgmt_tx nl80211_mgmt_tx.c
aarch64-linux-gnu-gcc -static -O2 -o mon_inject_raw mon_inject_raw.c
```

Push to device:

```sh
adb push nl80211_probe nl80211_probe_tx nl80211_mgmt_tx \
        mon_inject_raw /data/local/tmp/
```

## Usage

- `nl80211_probe addmon mon0` — create monitor interface.
- `nl80211_probe del mon0` — delete monitor interface.
- `nl80211_probe_tx <iface> <src-mac> <freq> <ssid> [wait]` — bounded
  probe request via NL80211_CMD_FRAME.
- `nl80211_mgmt_tx <iface> <freq> <frame-file-or-hex>` — arbitrary
  management frame via NL80211_CMD_FRAME (DONT_WAIT_FOR_ACK).
- `mon_inject_raw <iface> <frame-file-or-hex> [count] [interval_ms]` —
  raw frame via AF_PACKET with an 8-byte radiotap header on the monitor
  netdev (radiotap of any length >= 8 accepted by the driver).

## Kernel gates (all default N)

- `/sys/module/qca6490/parameters/monitor_probe_tx`
- `/sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev`
- `/sys/module/qca6490/parameters/monitor_mgmt_tx_unrestricted`
- `/sys/module/qca6490/parameters/monitor_mgmt_tx_chanfreq`
- Telemetry: `monitor_probe_tx_status`, `mgmt_tx_compare_enable/status`,
  `mon_chan_store_stage`, `mon_chan_set_stage`, `mon_xmit_stage`.

## True monitor conparam (channel set requires it)

```sh
svc wifi disable
echo 4 > /sys/module/qca6490/parameters/con_mode
# addmon/open triggers psoc re-init; wlan0 becomes the monitor interface
/data/local/tmp/nl80211_probe addmon mon0 || true
ip link set wlan0 up
echo '5180 0' > /sys/class/net/wlan0/monitor_mode_channel
```

## ABI reference

`qca-abi-reference-e0e27f32.versions` — raw `__versions` section of the
camera-safe baseline qca module (sha256
`e0e27f32aaa5c909ae1c06a7f46815df6e7756d14da18830ac67707c007d5f32`).
New module builds must keep the same (name, crc) import set; the raw
section byte order may shift with link order, so compare name/CRC pairs.
