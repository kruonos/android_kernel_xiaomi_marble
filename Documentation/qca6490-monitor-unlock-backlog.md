# QCA6490 Monitor TX Unlock Backlog

## Scope And Current State

This backlog separates work that can still be validated or implemented in the
host driver from the monitor-vdev restriction already attributed to production
QCA6490 firmware.

Completed host work:

- `mon0` raw TX parses radiotap, enters the driver TX path, and reaches
  `WMI_MGMT_TX_SEND`.
- Monitor cfg80211 management TX subtypes and monitor TX queue/carrier support
  are enabled.
- The connected STA route can submit management frames on its home channel.
- The root-only direct STA trigger can preserve a caller-supplied source MAC.

Known result:

- On the monitor vdev, probe, deauthentication, and QoS/data-shaped frames
  reached firmware but completed with `WMI_MGMT_TX_COMP_TYPE_DISCARD`.
- On the connected STA vdev, own-source and foreign-source management frames
  completed with `COMPLETE_OK`.

Do not treat a firmware `COMPLETE_OK` as proof of over-air delivery or peer
ACK. Do not run RF tests except on an authorized isolated AP/channel with a
separate receiver. Keep all module gates disabled after each test. Do not
modify or patch vendor firmware.

## 1. Capture STA Management TX Over The Air

Classification: external test infrastructure. No kernel change required.

Goal: independently verify over-air emission for the existing direct STA
management-TX route, for both own-source and foreign-source frames.

Prerequisites:

- An authorized test AP and an independent monitor-capable receiver on the
  STA home channel.
- Frame capture with accurate timestamps and no unrelated test clients.
- Current known-good boot/vendor_dlkm artifacts and recovery backup preserved.

Method:

1. Capture a baseline association exchange from the test phone.
2. Run one bounded own-source management-frame test using the existing gated
   STA route.
3. Correlate the device completion counter, receiver capture, and AP event.
4. Repeat once with the v16/v17 direct trigger and a foreign source address.
5. Restore all module parameters to `N` and confirm normal association.

Success criteria:

- Receiver observes the expected frame on the expected channel at the test
  time.
- Device shows one corresponding `COMPLETE_OK` completion.
- The AP behavior is recorded separately; firmware completion alone is not
  sufficient.

Stop conditions:

- Unexpected disconnects, firmware crash/restart, stuck TX completion, or
  frames observed outside the authorized channel.

## 2. Classify Non-Management Bytes On The STA WMI TX Path

Classification: firmware-dependent validation. No host implementation is
required before the first bounded trial.

Goal: determine whether the existing direct unrestricted trigger merely
accepts non-management-looking bytes or can cause them to be emitted over the
air.

Relevant behavior:

- The normal nl80211 route rejects non-management frames.
- The root-only direct trigger does not validate the 802.11 frame type before
  passing the bounded payload to the WMI management-TX path.

Method:

1. Use the isolated setup from item 1.
2. Send one minimal, bounded data-shaped test payload through the existing
   direct STA trigger.
3. Record `monitor_probe_tx_status`, WMI comparison telemetry when enabled,
   AP logs, and the independent capture.
4. Restore all gates immediately after the one-frame test.

Interpretation:

- `DISCARD` or no receiver capture means this is not a viable data-frame TX
  path.
- `COMPLETE_OK` without capture remains inconclusive.
- Only a matching receiver capture establishes over-air emission. It still
  does not establish usable general data injection or peer acceptance.

Stop conditions:

- Any WLAN firmware restart, host crash, persistent association failure, or
  unexpected frame class in the receiver capture.

## 3. Prototype A Separate STA Data-Path Test Only If Item 2 Fails

Classification: host implementation plus firmware/peer validation. Medium-high
risk.

Rationale: `WMI_MGMT_TX_SEND` is a management-TX engine. A data-shaped payload
accepted by its host entry point is not evidence that the firmware supports it
as ordinary 802.11 data TX.

Candidate direction:

- Use the existing QCA non-standard data TX flow in `wma_data.c` rather than
  extending the monitor WMI management-TX hook.
- Keep the prototype root-only, bounded to one associated STA and its exact
  home channel, disabled by default, and limited to a single test frame.

Required before editing:

- Inspect the complete ownership and completion-callback behavior of the
  existing data path.
- Have a kernel-detail review of the smallest proposed patch.
- Build one flashable package only, preserve the known-good package, and
  verify package integrity before any device test.

Success criteria:

- No ownership leak or driver/firmware recovery event.
- Expected completion behavior plus independent receiver capture.

## 4. Compare The Existing P2P Off-Channel Management Route

Classification: host instrumentation and firmware-dependent A/B experiment.

Goal: determine whether an existing supported non-monitor vdev route produces
different WMI parameters or firmware behavior than the rejected monitor-vdev
route.

Method:

1. Start with read-only WMI record comparison; do not alter P2P policy.
2. Compare vdev type, channel field, peer selection, flags, and completion
   status between the supported P2P route and the failing monitor route.
3. Only after review, consider a single isolated management-frame test using
   the normal P2P machinery.

This can identify a supported management path but does not unlock `mon0` raw
transmission.

## 5. Monitor-Vdev Unlock Decision Fence

Classification: firmware-dependent blocker.

The host path has already demonstrated that raw monitor input reaches
`WMI_MGMT_TX_SEND`. Production firmware returns `DISCARD` for monitor-vdev
traffic regardless of tested frame type and tested channel field.

Do not add further monitor netdev, cfg80211, carrier, radiotap, or channel-set
changes solely to try to overcome this result. Re-open monitor-vdev TX work
only when one of these exists:

- Vendor-provided or authorized test firmware that permits monitor-vdev TX.
- A documented, supported firmware transport that transmits from a monitor
  vdev without using the rejected management-TX command.

## Evidence To Record For Every Test

- Kernel/module hash and active slot.
- Exact gate values before and after the test.
- Frame class, length, channel, and test timestamp.
- `monitor_probe_tx_status` and any WMI comparison record.
- Independent receiver capture and AP-side event record.
- WLAN stability result and recovery steps, if any.

## Source References

- Monitor and STA common TX path:
  `drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_tx_rx.c`
- STA nl80211 routing restrictions:
  `drivers/staging/qcacld-3.0/core/hdd/src/wlan_hdd_p2p.c`
- Existing non-standard data path:
  `drivers/staging/qcacld-3.0/core/wma/src/wma_data.c`
- Monitor-vdev discard and STA-vdev experiment results:
  `Documentation/qca6490-mgmt-tx-firmware-map.md`

## Publication And Distribution Plan (parked)

Planned standalone repository `qca6490-mgmt-inject`:

- `patches/`: clean `git format-patch` series (experiment telemetry commits
  squashed out), applicable to any qcacld-3.0 tree.
- `module/`: KernelSU module template (module.prop + service.sh +
  prebuilt qca_cld3_qca6490.ko for the Bouquet 5.10.258-v4.9 kernel).
- `tools/`: existing userspace tools and device scripts.
- `docs/`: firmware map, limitations, backlog, README with build recipe
  for other kernels and the own-hardware scope framing.

KernelSU module notes:

- All feature changes live inside qca_cld3_qca6490.ko; the boot Image and
  cfg80211 stay untouched, so a module-only install is sufficient for
  matching-kernel users.
- Cross-kernel portability requires rebuilding the module per kernel
  (vermagic and symbol CRCs); the patchset is the portable artifact.
- Module reload mechanics: disable Wi-Fi, unload/load the driver module,
  re-enable (or reboot with early mount).

Status: parked until operator requests assembly.

## Progress Log

### 2026-08-25- Item 2 executed on the connected STA (PRINT POST 5G, 5745 MHz) through
  the direct trigger:
  - QoS data-shaped frame (fc=0x8800, 26 bytes, DA=AP, SA=own, BSSID=AP):
    firmware `COMPLETE_OK` (`status=0`). The management TX engine accepted
    data-shaped bytes; without an independent receiver this remains
    inconclusive for over-air emission.
  - Control-shaped RTS (fc=0xb400, 16 bytes): firmware `COMPLETE_NO_ACK`
    (`status=3`); the recorded WMI frame bytes were already zeroed/mangled
    at record time (fc=00, zero addresses) — do not treat this as a valid
    control-frame result.
- Item 1 blocked by operator decision: the notebook Wi-Fi adapter was
  disruptive when switched to monitor mode; assume no second
  monitor-capable device is available. All over-air capture work is
  suspended until a dedicated receiver exists.
- Item 3 not triggered: item 2 did not fail at the firmware-verdict level.
- Item 4 read-only comparison data already recorded (normal driver
  management path vs custom path fields); no P2P off-channel test was run
  because it needs the P2P remain-on-channel machinery, which stays
  untouched per this backlog.
- Item 5 fence respected: no further monitor-vdev TX changes made.
