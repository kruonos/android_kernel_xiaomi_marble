#!/system/bin/sh
set -u

cleanup() {
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
    /data/local/tmp/nl80211_probe del mon0 || true
}

trap cleanup EXIT INT TERM

/data/local/tmp/nl80211_probe del mon0 || true
/data/local/tmp/nl80211_probe addmon mon0 || exit 1
ip link set mon0 up || exit 1
printf '5180 0\n' > /sys/class/net/mon0/monitor_mode_channel
printf 'Y' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
printf 'Y' > /sys/module/qca6490/parameters/monitor_probe_tx

sleep 5
cmd wifi status

mac=$(cat /sys/class/net/mon0/address)
/data/local/tmp/nl80211_probe_tx mon0 "$mac" 5180 VIVOFIBRA-2250-5G
ret=$?
sleep 2
cat /sys/module/qca6490/parameters/monitor_probe_tx_status
exit "$ret"
