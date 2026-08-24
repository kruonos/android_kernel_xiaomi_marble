#!/system/bin/sh
set -u

# monitor_v13_diag.sh <freq>
freq=${1:-5180}

svc wifi disable
sleep 1
echo 4 > /sys/module/qca6490/parameters/con_mode
/data/local/tmp/nl80211_probe del mon0 || true
/data/local/tmp/nl80211_probe addmon mon0 || true
sleep 6
ip link set wlan0 up || true
sleep 2
ip -br link show wlan0
dmesg -c > /dev/null
printf '%s 0\n' "$freq" | dd of=/sys/class/net/wlan0/monitor_mode_channel 2>&1
sleep 1
dmesg | grep -E "mon_chan" | tail -20
echo "set_stage=$(cat /sys/module/qca6490/parameters/mon_chan_set_stage)"
echo "store: $(cat /sys/module/qca6490/parameters/mon_chan_store_stage)"
