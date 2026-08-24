#!/system/bin/sh
set -eu

freq=${1:-5200}
ssid=${2:-VIVOFIBRA-2250-5G}
route=${3:-monitor}

cleanup() {
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
    /data/local/tmp/nl80211_probe del mon0 || true
}

trap cleanup EXIT INT TERM

/data/local/tmp/nl80211_probe del mon0 || true
/data/local/tmp/nl80211_probe addmon mon0
ip link set mon0 up
printf '%s 0\n' "$freq" > /sys/class/net/mon0/monitor_mode_channel

if [ "$route" = sta ]; then
    printf 'Y' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
else
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
fi

mac=$(cat /sys/class/net/mon0/address)
before_packets=$(cat /sys/class/net/mon0/statistics/tx_packets)
before_bytes=$(cat /sys/class/net/mon0/statistics/tx_bytes)
printf 'Y' > /sys/module/qca6490/parameters/monitor_probe_tx

/data/local/tmp/nl80211_probe_tx mon0 "$mac" "$freq" "$ssid"
sleep 2

after_packets=$(cat /sys/class/net/mon0/statistics/tx_packets)
after_bytes=$(cat /sys/class/net/mon0/statistics/tx_bytes)
printf 'mon0_tx_packets=%s->%s mon0_tx_bytes=%s->%s\n' "$before_packets" "$after_packets" "$before_bytes" "$after_bytes"
dmesg | grep -F 'monitor probe tx' | tail -n 4 || true
