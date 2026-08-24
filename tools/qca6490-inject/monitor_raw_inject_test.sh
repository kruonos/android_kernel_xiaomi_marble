#!/system/bin/sh
set -u

# usage: monitor_raw_inject_test.sh <freq> <frame-file-or-hex> [netdev|nl] [count] [interval_ms]
freq=${1:-5180}
frame=${2:-}
route=${3:-netdev}
count=${4:-1}
interval_ms=${5:-0}

cleanup() {
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
    printf 'N' > /sys/module/qca6490/parameters/monitor_mgmt_tx_unrestricted
    /data/local/tmp/nl80211_probe del mon0 || true
}

trap cleanup EXIT INT TERM

/data/local/tmp/nl80211_probe del mon0 || true
/data/local/tmp/nl80211_probe addmon mon0 || exit 1
ip link set mon0 up || exit 1
printf '%s 0\n' "$freq" > /sys/class/net/mon0/monitor_mode_channel

printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
printf 'Y' > /sys/module/qca6490/parameters/monitor_probe_tx
printf 'Y' > /sys/module/qca6490/parameters/monitor_mgmt_tx_unrestricted

if [ "$route" = nl ]; then
    /data/local/tmp/nl80211_mgmt_tx mon0 "$freq" "$frame"
else
    /data/local/tmp/mon_inject_raw mon0 "$frame" "$count" "$interval_ms"
fi
ret=$?
sleep 3
cat /sys/module/qca6490/parameters/monitor_probe_tx_status
exit "$ret"
