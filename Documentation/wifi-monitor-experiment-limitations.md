# QCA6490 Monitor / Management TX Experiment Limitations

## What Works

With the custom kernel gates enabled by root, a connected `wlan0` STA vdev
can submit crafted IEEE 802.11 management frames through
`NL80211_CMD_FRAME` to the QCA6490 firmware management-TX path. The device
telemetry recorded three firmware `COMPLETE_OK` completions on the STA vdev.

This includes management subtypes such as authentication, deauthentication,
and probe requests, provided the device is associated and the request uses its
current home channel.

## What It Does Not Provide

This is not unrestricted raw Wi-Fi injection.

- It cannot inject normal 802.11 data frames through the working route.
- It cannot transmit off-channel. The STA must be connected, associated, and
  use its exact current home channel.
- It cannot use an arbitrary source MAC through the working STA/cfg80211
  route. The driver replaces the source address with the connected STA MAC,
  and cfg80211 rejects invalid management-frame source addresses.
- It cannot transmit arbitrary monitor-mode frames over the air. The host
  monitor path can accept raw radiotap-framed input, but production QCA6490
  firmware returns `WMI_MGMT_TX_COMP_TYPE_DISCARD` for monitor-vdev TX.
- It cannot establish that a frame was acknowledged by a peer. The tools use
  `DONT_WAIT_FOR_ACK`; `COMPLETE_OK` means firmware accepted and completed
  the WMI management-TX request, not that an independent receiver observed
  the frame or responded to it.
  Note: over-air transmission of the deauthentication frame was indirectly
  observed (the AP dropped the client and the station reconnected with a new
  MAC address), but no ACK-level telemetry exists.
- It is not available to ordinary Android applications. The controls are
  root-only module parameters in a custom QCA driver.

## Effective Security Boundary

The effective capability is: a root operator of this modified phone can cause
its already-associated Wi-Fi station to emit selected management frames on its
current channel using its own station identity. This can be disruptive to that
station's connection and is useful for controlled AP/firmware testing, but it
is not a general-purpose wireless spoofing, arbitrary-client impersonation,
or raw data-packet injection platform.

## Source Evidence

- STA routing and same-channel management-only checks:
  `WIFI-MONITOR-EXPERIMENT/drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_p2p.c:307-330`
- Frame bounds, connected-STA checks, source-address replacement, and WMI TX
  submission:
  `WIFI-MONITOR-EXPERIMENT/drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_tx_rx.c:245-388`
- Firmware completion telemetry:
  `WIFI-MONITOR-EXPERIMENT/drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_tx_rx.c:158-224`
- Observed monitor-vdev firmware discard and successful STA-vdev tests:
  `WIFI-MONITOR-EXPERIMENT/Documentation/qca6490-mgmt-tx-firmware-map.md:483-538`
