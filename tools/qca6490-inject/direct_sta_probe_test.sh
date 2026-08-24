#!/system/bin/sh
set -u

cleanup() {
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx
    printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
}

trap cleanup EXIT INT TERM

printf 'Y' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
printf 'Y' > /sys/module/qca6490/parameters/monitor_probe_tx

/data/local/tmp/nl80211_probe_tx wlan0 5e:a2:44:55:18:8c 5745 "PRINT POST 5G"
ret=$?
sleep 2
cat /sys/module/qca6490/parameters/monitor_probe_tx_status
exit "$ret"
