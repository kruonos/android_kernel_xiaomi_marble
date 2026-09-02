# QCA6490 Monitor / Management TX Experiment Limitations

## What Works

With the custom kernel gates enabled by root, a connected `wlan0` STA vdev
can submit crafted IEEE 802.11 management frames through
`NL80211_CMD_FRAME` to the QCA6490 firmware management-TX path. The device
telemetry initially recorded three firmware `COMPLETE_OK` completions on the
STA vdev. The later v16/v17 direct-trigger experiment recorded four successful
completions, including foreign-source probe and deauthentication frames.

This includes management subtypes such as authentication, deauthentication,
and probe requests, provided the device is associated and the request uses its
current home channel.

## What It Does Not Provide

This is not unrestricted raw Wi-Fi injection.

- The normal `NL80211_CMD_FRAME` route rejects non-management frames. The
  root-only `monitor_spoof_tx` test hook can pass non-management-looking
  802.11 bytes into the WMI management-TX path with unrestricted gates
  enabled, but over-air data-frame injection is not established.
  Note: the v20 DP exception path (`monitor_spoof_tx_data`) DID establish
  raw unencrypted data-frame transmission with caller-chosen headers on
  the associated STA vdev, proven by victim-side iwlwifi counter deltas
  (2026-09-02). See the firmware map, Step M.
- It cannot transmit off-channel. The STA must be connected, associated, and
  use its exact current home channel.
- The normal STA/cfg80211 route cannot use an arbitrary source MAC: cfg80211
  rejects invalid management-frame source addresses and the driver replaces
  the source address with the connected STA MAC. A separate root-only direct
  trigger (`monitor_spoof_tx`) bypasses cfg80211, and
  `monitor_mgmt_tx_spoof_sa` preserves the caller-provided source address.
  The v16/v17 experiment recorded firmware `COMPLETE_OK` for foreign-source
  probe and deauthentication frames. The nl80211 route still enforces the
  interface MAC.
- It cannot transmit arbitrary monitor-mode frames over the air. The host
  monitor path can accept raw radiotap-framed input, but production QCA6490
  firmware returns `WMI_MGMT_TX_COMP_TYPE_DISCARD` for monitor-vdev TX.
- It cannot establish that a frame was acknowledged by a peer. The tools use
  `DONT_WAIT_FOR_ACK`; `COMPLETE_OK` means firmware accepted and completed
  the WMI management-TX request, not that an independent receiver observed
  the frame or responded to it.
  Note: earlier own-source deauthentication testing indirectly showed an
  AP-side effect when the client disconnected and reconnected. In the later
  foreign-source test, firmware returned `COMPLETE_OK`, but the AP rejected
  the unassociated source and the phone stayed connected.
- It is not available to ordinary Android applications. The controls are
  root-only module parameters in a custom QCA driver.

## Effective Security Boundary

The effective capability is: a root operator of this modified phone can cause
its already-associated Wi-Fi station to submit selected same-channel frames to
the WMI management-TX path, including caller-provided source addresses when
the direct spoof trigger and spoof-SA gate are enabled. Firmware accepted
foreign-source management frames in the documented v16/v17 test.

This remains constrained by root-only controls, association, the STA home
channel, firmware and AP behavior, and the lack of ACK-level proof. It has not
established a general raw data-packet injection capability, but it is a real
management-frame source-address spoofing capability for controlled testing.

## Source Evidence

- STA routing and same-channel management-only checks:
  `drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_p2p.c:304-330`
- Direct spoof trigger:
  `drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_tx_rx.c:160-232`
- Frame bounds, connected-STA checks, optional source-address replacement,
  and WMI TX submission:
  `drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_tx_rx.c:303-488`
- Firmware completion telemetry:
  `drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_tx_rx.c:269-300`
- Observed monitor-vdev firmware discard and successful STA-vdev tests:
  `Documentation/qca6490-mgmt-tx-firmware-map.md:483-568`
