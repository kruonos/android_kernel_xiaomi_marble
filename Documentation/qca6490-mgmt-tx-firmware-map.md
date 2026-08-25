# QCA6490 Firmware Mgmt-TX Discard Map and Action Guide

Date: 2026-08-21
Scope: map every firmware-side point related to the monitor probe-request TX
discard, and document the next steps. No firmware modification is planned.

## 1. Current state (runtime fact)

- Device: POCO F5 / marble, kernel `5.10.258-Bouquet-v4.9`, QCA6490 driver
  `qca_cld3_qca6490.ko`.
- One broadcast probe request sent via `NL80211_CMD_FRAME` on `mon0`
  (freq 5200, 43 bytes, `DONT_WAIT_FOR_ACK`).
- Kernel telemetry (`/sys/module/qca6490/parameters/monitor_probe_tx_status`):

  ```text
  queued=1 completed=1 pending=0 complete_ok=0 discard=1 inspect=0 no_ack=0 other=0 last_status=1
  ```

- Meaning: the frame reached the firmware; the firmware completed the
  management-TX with `WMI_MGMT_TX_COMP_TYPE_DISCARD` (status `1`). The frame
  was NOT transmitted over the air. This is a firmware-side rejection.
- v9 repeated the same bounded probe directly through connected `wlan0`, with
  no monitor interface, on the STA's exact active channel. Firmware again
  returned `DISCARD`:

  ```text
  queued=1 completed=1 complete_ok=0 discard=1 last_status=1 last_sta_vdev=1
  ```

  Therefore the rejection is not caused specifically by the monitor vdev.
- v11 changed only the firmware channel field from the explicit frequency to
  `0` (meaning: use the connected STA's active channel). The same broadcast
  probe then completed successfully:

  ```text
  queued=1 completed=1 complete_ok=1 discard=0 last_status=0 last_sta_vdev=1
  ```

  Confirmed root cause: `WMI_MGMT_TX_SEND` rejects an explicit channel for
  this in-band connected-STA probe path. It requires channel 0.

## 2. Firmware files (pulled from the device)

Device source: `/vendor/firmware_mnt/image/qca6490/` (loaded via cnss2, see
`dmesg | grep cnss`).

Local copies: `/tmp/kilo/fw-qca6490/`

| File | Role | SHA-256 |
|---|---|---|
| `amss20.bin` | main WLAN firmware (loaded; ELF32 ARM, no sections) | `903c378a972b4294fef5c926e34c2cfb03eb042f6a1e38101f0073b33792df86` |
| `amss.bin` | alternate main firmware build | `a626cfc1882b3e386154fac655652005e8f20ff0176b4c63de99cd082d5d75c2` |
| `m3.bin` | M3 payload carrier (ELF32 wrapper, payload is Thumb-2) | `ad234ac8879e29a113893b87dc90335cff8eb89717580d5a06a49e7b9e322ed` |
| `Data20.msc` | debug-message catalog for `amss20.bin` (10,721 entries) | `3c87a16e6c6814b5356ead716b87af5c14c32efba113fe51b2600e44ad126c41` |
| `Data.msc` | debug-message catalog for `amss.bin` | `31d5fee12d94bf86841c4c06d3460959373f3468ea4ca0f13d7da67de739a0cc` |
| `bd_m16tgfgl.elf` | loaded board-data file (BDF) | `e0d1ca0d77e6bd310111b174cee40de434e004e270210829332f0938a08e725d` |
| `regdb_xiaomi.bin` | regulatory database (loaded) | (see dir) |

The catalog tells us the exact firmware source files and line numbers of the
mgmt-TX path (see section 5).

## 3. WMI interface facts (verified against this kernel tree)

Source: `drivers/staging/fw-api/fw/wmi_unified.h`

- Command: `WMI_MGMT_TX_SEND_CMDID = 0x7008`
  (`WMI_CMD_GRP_START_ID(WMI_GRP_MGMT=0x7)` = `0x7001`, +7 entries).
- Completion event: `WMI_MGMT_TX_COMPLETION_EVENTID = 0x7006`.
- TLV tags (what actually travels over WMI):
  - `WMITLV_TAG_STRUC_wmi_mgmt_tx_send_cmd_fixed_param = 0x1a8` (command)
  - `WMITLV_TAG_STRUC_wmi_mgmt_tx_compl_event_fixed_param = 0x1a9` (event)
  - TLV header word = `(tag << 16) | (len & 0xffff)`
    (`wmi_tlv_helper.h:56 WMITLV_SET_HDR`).
- Status enum (`wmi_unified.h:11154`):
  `COMPLETE_OK=0, DISCARD=1, INSPECT=2, COMPLETE_NO_ACK=3`.
- Command fixed param `wmi_mgmt_tx_send_cmd_fixed_param`
  (`wmi_unified.h:8383`): `vdev_id, desc_id, chanfreq, paddr_lo, paddr_hi,
  frame_len, buf_len, tx_params_valid, tx_flags, peer_rssi` + `bufp[]` +
  optional `wmi_tx_send_params`.
- Completion fixed param (`wmi_unified.h:11172`): `desc_id, status, pdev_id,
  ppdu_id, ack_rssi, tx_rate, peer_phymode, retries_count, tx_tsf_l32,
  tx_tsf_u32, info` (44 bytes).
- Service bits: `WMI_SERVICE_MGMT_TX_WMI = 76` (`wmi_services.h:124`) — the
  firmware must have advertised this service (it did, otherwise the host would
  not use the WMI path and we would not receive a WMI completion).
- Host send path: `wlan_hdd_tx_rx.c hdd_mon_probe_mgmt_tx()` ->
  `wlan_mgmt_txrx_mgmt_frame_tx()` ->
  `wma_mgmt_unified_cmd_send()` (`wma_mgmt.c:4038`) ->
  `send_mgmt_cmd_tlv()` (`wmi_unified_tlv.c:3807`) -> CE.
- Host receive path: `wma_mgmt_tx_completion_handler()` (`wma_mgmt.c:2835`)
  -> `mgmt_txrx_tx_completion_handler()` -> our OTA callback.

The host side is fully mapped and works; the discard originates in firmware.

## 4. Static binary map of `amss20.bin`

ELF32 ARM, image segments (from program headers):

| File offset | VA | Size | Flags |
|---|---|---|---|
| 0x002000 | 0x01424000 | 0x12604 | R E |
| 0x015000 | 0x01440000 | 0x0b4ac | RWE |
| 0x021000 | 0x01710000 | 0x046c8 | RW |
| 0x026000 | 0x01470000 | 0x03104 | R E |
| 0x02a000 | 0x01474000 | 0x0145c | RW |
| 0x02c000 | 0x0146b000 | 0x04c68 | R E |
| 0x031000 | 0x014b0000 | 0x120000 | R E |
| 0x151000 | 0x015d0000 | 0x1b3c8 | R E |
| 0x16d000 | 0x015ec000 | 0x1578c | RW |
| 0x183000 | 0x01400140 | 0x0e340 | RW |
| 0x192000 | 0x01705000 | 0x207000 | R E |
| 0x399000 | 0x0190c000 | 0x2a000 | R |
| 0x3c3000 | 0x01936000 | 0x12b000 | RW |

Entry point: `0x1436500`.

Tag reference points found byte-exactly (file offset -> VA):

| Tag | Meaning | File offset | VA |
|---|---|---|---|
| 0x1a8 | mgmt-tx-send flags/parse descriptor | 0x157b10 | 0x15d6b10 |
| 0x1a8 | mgmt-tx-send TLV parse descriptor | 0x3b15d0 | 0x19245d0 |
| 0x1a9 | completion event descriptor | 0x3abf70 | 0x191ef70 |
| 0x1a9 | completion event descriptor (2nd table) | 0x3b492c | 0x192792c |

WMI event-id table (`{index, 0x1280xxxx}` pairs, 159 entries) starts near
`0x15e1f44`; the mgmt-TX-completion entry is `{0x18, 0x128001A9}` at
VA `0x15e2044` (file 0x163044). The 0x1280 prefix marks firmware WMI event
IDs; low 16 bits = TLV tag.

No code in `amss20.bin` loads tags 0x1a8/0x1a9 as immediates: the TLV
encoder is table-driven and reads the tag from the descriptor tables above
(verified by full ARM+Thumb immediate sweep). This is why locating the
completion builder requires either Ghidra cross-references to the descriptor
tables or runtime logging (section 7).

### M3 payload (`m3.bin`)

- `m3.bin` is an ELF wrapper; the real image is 0x40000 bytes at file offset
  0x1000 (extracted as `/tmp/kilo/fw-qca6490/m3.payload.bin`).
- The payload is ARM Thumb-2 code (despite the wrapper claiming EM_386):
  vector table at offset 0: SP `0x3fff0`, reset `0x401`, handlers `0x44d`,
  `0x42d`, `0xb5cd`, `0xb5c9` (odd = Thumb).
- String `"bad descr_idx"` at payload offset `0x29fd8` — the M3 validates
  management-TX descriptor indices and rejects bad ones. No direct
  reference found yet (string is likely addressed via the M3 debug/string
  tables, not a literal pool).

## 5. Firmware mgmt-TX trace points (from `Data20.msc` catalog)

The catalog maps message id -> file:line -> format string for `amss20.bin`.
The relevant firmware sources are `wlan_mgmt_txrx.c` (WMI mgmt-TX handler +
completion sender) and `ar_wal_local_frames.c` (WAL local-TX engine, which
generates the completion status).

`wlan_mgmt_txrx.c` (message ids in Data20.msc):

| id | line | What it logs |
|---|---|---|
| 3714 | 1642 | MGMT_TX_WMI_FROM_HOST: home/current channel modes, mac_id |
| 3713 | 1734 | MGMT_TX_WMI_FROM_HOST: subtype, addr, chanfreq, peer_rssi |
| 3712 | 1739 | MGMT_TX_WMI_FROM_HOST: check_tag, tx_param_valid, cmd_ch (channel), frame_len, tx_flags, desc_id, tid_num, ppdu_info |
| 3699 | 1318 | MGMT_TX_WMI_FROM_HOST: dword0/dword1 (tx params), subtype, ppdu_info |
| 3700 | 1994 | MGMT_TX_WMI_FROM_HOST_COMP: desc_id, tx_status (single event) |
| 3704 | 2119 | MGMT_TX_WMI_FROM_HOST_COMP: desc_id, tx_status, ppdu_id, ack_rssi |
| 3707 | 1963 | bundle completion: num, desc_id, comp |
| 3709 | 2261 | WMI TX comp hold: idx, desc id, subtype, comp |
| 3702/3703 | 878/965 | WMI_SEND_EVENT_WRONG_TLV |

`ar_wal_local_frames.c` (WAL):

| id | line | What it logs |
|---|---|---|
| 924 | 231 | MGMT_TX_WMI_FROM_HOST TX COMP from lower layer (flags, buffer_id, fc) |
| 923 | 384 | WAL_DBGID_TX_MGMT_COMP_DESCID_STATUS: vdev_id, tx_status |
| 919 | 380 | same, extended: vdev_id, mac_id, desc_id, tx_status, tid, buffer_id, reset_cnt |
| 917 | 1151 | WAL_DBGID_TX_MGMT_ENQUEUE_FAILED: vdev_id, tid |
| 916 | 1175 | Local mgmt send: requested TID, tid_alloc_status |
| 915 | 1179 | "tid isn't pre-allocated", qpeer_flags |
| 914 | 1391 | WAL_DBGID_TX_MGMT_DESCID_SEQ_TYPE_LEN: vdev_id, mac_id, cookie, seq, type, len |
| 912/911 | 2548/2578 | send local buffer fail (migration in progress / status+flags) |
| 909/906 | 2456/2073 | local_mgmt / host_mgmt peer pdev mismatch |
| 908 | 2018 | WAL_DBGID_TX_MGMT_ENQUEUE_FAILED (2nd site) |
| 905 | 2246 | DESCID_SEQ_TYPE_LEN (2nd site) |
| 903 | 2303 | WAL_DBGID_MGMT_TX_FAIL: vdev_id, err_code, cookie, status, fc[0] |

Interpretation of the completion flow inside firmware:

1. WMI layer (`wlan_mgmt_txrx.c:1642-1739`) validates the command
   (check_tag, tx params, channel `cmd_ch`, frame_len, tx_flags, desc_id,
   TID) and forwards to the WAL.
2. WAL (`ar_wal_local_frames.c`) allocates a TID and enqueues the local
   management frame. Enqueue failures are logged by 917/908/915.
3. On TX completion the WAL logs 924/923/919 and returns a `tx_status`.
4. `wlan_mgmt_txrx.c:2119` (log 3704) sends the WMI completion event
   (tag 0x1a9) with that `tx_status` back to the host.

The DISCARD status we saw is set at one of these WAL/M3 steps.

## 6. Confirmed discard cause

Runtime A/B testing confirmed the cause:

1. v9 sent explicit `chanfreq=5745` on a connected STA vdev: firmware status 1
   (`DISCARD`).
2. v11 kept the same frame, STA vdev, self peer, broadcast addresses, flags,
   and parameter state, but sent `chanfreq=0`: firmware status 0
   (`COMPLETE_OK`).
3. Therefore peer choice, host lane metadata, host `use6` metadata, broadcast
   destination and missing optional completion metadata were not responsible
   for the firmware discard in this test.

## 7. What to do next

### Step A — firmware dbglog capture attempt (completed; no records)

The stock v7 module does not export the `dl_*` names in `SIOCGIWPRIV`, but the
underlying WEXT handlers are present. The base WEXT command is `0x8be0`; its
payload is two integers `{subcommand, value}`. The static aarch64 client
`/tmp/kilo/wlan_priv` implements this as:

```text
wlan_priv wlan0 raw <subcommand> <value>
```

The first draft of this helper encoded the two integers incorrectly. It was
corrected before the final capture; `tx_stbc` was restored to its INI value of
`1`. All results below use the corrected helper.

Relevant subcommands: `31=dl_loglevel`, `32=dl_vapon`, `33=dl_vapoff`,
`34=dl_modon`, `35=dl_modoff`, `36=dl_mod_loglevel`, `37=dl_type`, and
`40=dl_report`.

Completed runtime capture, 2026-08-21:

1. Restored `tx_stbc=1`, then used the correct WEXT ABI.
2. `raw 31 1` and `raw 40 1` both reached the QCA driver successfully.
3. The parser was tried in both raw-print (`raw 37 1`) and raw-netlink
   (`raw 37 3`) modes.
4. A direct generic-netlink listener was subscribed before the probe was sent:
   - family: `cld80211` = 32
   - group: `fw_logs` = 21
   - client: `/tmp/kilo/cld_fwlog_watch`
5. A single probe again completed as firmware `DISCARD`, but the subscribed
   listener received **zero** firmware-log messages. `dmesg` and Android log
   buffers also received zero QCA dbglog records.
6. `raw 40 0` disabled firmware reporting and `raw 37 3` restored the default
   netlink parser mode after the test.

Conclusion: the host accepts the dbglog configuration commands, but this
production firmware does not emit or forward a `WMI_DEBUG_MESG_EVENTID` record
for this path. The catalog remains valid static evidence, but cannot yet name
the runtime discard branch.

### Step B — STA-vdev A/B experiment

v8 adds a second default-off selector:

```text
/sys/module/qca6490/parameters/monitor_probe_tx_sta_vdev
```

- `N`: unchanged monitor-vdev path (known result: firmware DISCARD).
- `Y`: keep the same monitor entry checks and same broadcast probe, but submit
  it through connected `wlan0`. The kernel rewrites only the source MAC to
  `wlan0`'s MAC, requires `wlan0` connected/associated/up, and requires its
  active frequency to equal the monitor frequency.

The status output now includes `last_sta_vdev=0|1` so the tested path is
unambiguous.

v8 runtime result, 2026-08-21:

- `wlan0` was connected to `VIVOFIBRA-2250-5G` on 5180 MHz before the test.
- Creating and bringing up `mon0` disconnected `wlan0`; it remained
  disconnected while `mon0` existed.
- The simultaneous STA selector correctly rejected the request with `ENODEV`.
- Telemetry remained `queued=0 completed=0`; the frame never reached firmware.
- Therefore this platform cannot perform the A/B test with monitor and active
  STA vdevs at the same time, despite advertising both interfaces.

A direct `NL80211_CMD_FRAME` on `wlan0` was superficially accepted by the
generic path, but that path returns success without propagating its internal
TX result and emitted no status event. That acceptance was not proof of TX.

v9 fixes the experiment by intercepting the same constrained probe directly
on connected `wlan0`; `mon0` is not created. Both experimental parameters must
be `Y`, otherwise normal STA cfg80211 traffic is untouched.

v9 post-flash test:

1. Keep `wlan0` connected to the owned AP and read its current frequency and
   source MAC.
2. Set `monitor_probe_tx_sta_vdev=Y` and `monitor_probe_tx=Y`.
3. Send exactly one existing bounded probe through `wlan0`, using the active
   STA MAC and active frequency.
4. Read `monitor_probe_tx_status` after two seconds.

Interpretation:

- `last_sta_vdev=1 complete_ok` increases: firmware supports the packet and
  rejects only the monitor-vdev setup.
- `last_sta_vdev=1 discard` increases: rejection is not limited to monitor
  vdev; compare WMI params/peer/TID next.
- No queued increase: the kernel rejected the test because STA was not
  connected/up or the frequencies did not match.

v9 runtime result, 2026-08-21:

- Connected STA: `PRINT POST 5G`, BSSID `78:3e:a1:d4:fa:3d`, 5745 MHz.
- Sent one 39-byte broadcast probe directly on `wlan0`; `mon0` did not exist.
- Result:

  ```text
  queued=1 completed=1 pending=0 complete_ok=0 discard=1
  last_status=1 last_sta_vdev=1
  ```

- `wlan0` remained connected after the test.
- Conclusion: firmware rejects the custom probe even through a valid connected
  STA vdev. The next comparison must focus on WMI TX parameters, peer/TID
  selection, and frame requirements rather than monitor-vdev setup.

v9 artifact:

```text
Bouquet-5.10.258-Bouquet-v4.9-marble-monitor-probe-tx-v9-direct-sta-ab.zip
SHA-256 d0d0e5664a18613e473cc4fca632f8f9c6f9870535e52cd5a3cbc8b72edcebf4
```

### Step C — working-versus-rejected WMI recorder (implemented in v10)

Commit: `7f8e10838935 wifi: record management TX WMI comparisons`

The recorder is default-off and stores the latest 12 completed management
packets. It correlates each driver send with its firmware completion using the
firmware descriptor id. The dump is root-readable only because it contains MAC
addresses.

Controls:

```text
/sys/module/qca6490/parameters/mgmt_tx_compare_enable
/sys/module/qca6490/parameters/mgmt_tx_compare_status
```

Writing `Y` to `mgmt_tx_compare_enable` clears old records and starts a fresh
capture. Writing `N` stops and clears it.

Capture procedure:

1. Enable the recorder while `wlan0` is connected.
2. Trigger one normal reconnect to an owned AP without unloading the QCA
   module; this records normal authentication/association management packets.
3. Send one bounded direct-STA probe with both probe parameters enabled.
4. Read `mgmt_tx_compare_status`.

Each line shows, in one place:

- frame type (`fc`), connection id (`vdev`), channel, length;
- selected connection peer, destination, source and BSSID;
- whether explicit rate parameters were supplied (`tp`), flags, 6 Mbps mode;
- transmission lane (`tid=valid:value`), retry/rate/chain/bandwidth settings;
- firmware result (`status`: 0 success, 1 discard, 2 inspect, 3 no-ACK,
  -2 host-send failure);
- optional completion metadata (`meta`, completion frequency/rate/retries).

v10 artifact:

```text
Bouquet-5.10.258-Bouquet-v4.9-marble-monitor-probe-tx-v10-mgmt-compare.zip
SHA-256 822deddbf060d652f5cda91e8e746b12e4f5237842c609b3884f3a3a486b5e4c
```

v10 runtime comparison, 2026-08-24:

```text
                         Working probe request       Rejected custom probe
vdev                     0                           0
frame control            0x40                        0x40
firmware channel field   0 (use active STA channel)  5200
selected peer            AP d8:c6:78:f5:22:57        STA self peer
destination/BSSID        AP                          broadcast
explicit TX params       no                          no
TX flags                 0                           0
host lane metadata       valid:7                     absent
host 6 Mbps metadata     1                           0
firmware result          0 (success)                 1 (discard)
```

The normal reconnect produced seven successful management records: directed
probe request, authentication, association and action frames. All completed
with firmware status 0. The custom probe completed with status 1.

Important interpretation:

- `use6` and `tid` are useful host-side evidence but are not serialized into
  this target's `WMI_MGMT_TX_SEND` fixed parameters, so changing them alone
  would not test firmware behavior.
- The channel field is serialized. Normal in-band STA probe requests use 0,
  while the custom probe used 5200.
- Commit `3c5dcda02c31 wifi: use STA home channel for probe TX` changes only
  the direct-STA custom path to send channel 0 after still validating that the
  requested nl80211 frequency equals the active STA home channel.

v11 artifact:

```text
Bouquet-5.10.258-Bouquet-v4.9-marble-monitor-probe-tx-v11-sta-home-channel.zip
SHA-256 1131dc9b164231bc56266a2d05dc8ff1593fd8400368cc8b182a1d16acded70f
```

v11 runtime result, 2026-08-24:

```text
seq=1 desc=63 vdev=0 fc=40 len=39 freq=0
peer=5a:80:11:31:31:63 da=ff:ff:ff:ff:ff:ff
sa=5a:80:11:31:31:63 bssid=ff:ff:ff:ff:ff:ff
type=0 tp=0 flags=00000000 tid=0:0 status=0

queued=1 completed=1 complete_ok=1 discard=0
last_status=0 last_sta_vdev=1
```

This is the first successful completion of the bounded custom probe path.

### Step H — true monitor injection (v12)

v11 proved `WMI_MGMT_TX_SEND` completes when `chanfreq=0` on a connected STA.
v12 extends the same pattern to the monitor vdev and lifts the frame bounds
there, while the STA path stays bounded:

1. New gate `monitor_mgmt_tx_unrestricted` (default `N`), monitor vdev only.
2. Monitor vdev TX with the gate on accepts any frame type/subtype, any
   SA/DA/BSSID, 10..2048 bytes, no rate limit (soft cap: 64 in-flight WMI
   mgmt TX frames).
3. `chanfreq` is now always `0`; the firmware derives the channel from the
   active vdev. For `mon0` the vdev channel is the one set through
   `monitor_mode_channel` sysfs (`sme_roam_channel_change_req`).
4. The `mon0` netdev xmit path parses radiotap headers of any length >= 8
   instead of requiring an all-zero 8-byte header.
5. `NL80211_CMD_FRAME` on `mon0` accepts any management subtype while the
   gate is on.

Entry points:

- `mon0` AF_PACKET xmit (radiotap + frame): arbitrary 802.11 frames.
- `NL80211_CMD_FRAME` on `mon0`: arbitrary management frames.

Hypotheses to verify:

- firmware accepts `chanfreq=0` on the monitor vdev (v11 analog);
- firmware accepts non-broadcast and foreign-SA management frames;
- data frames through the WMI mgmt engine: unknown, observe status.

Runtime test plan:

1. `svc wifi disable`, create `mon0`, set channel via
   `monitor_mode_channel` sysfs.
2. Enable `monitor_probe_tx=Y` and `monitor_mgmt_tx_unrestricted=Y`.
3. Bounded probe sanity on `mon0` (both entry points).
4. Deauth/auth/action to the owned AP with a foreign SA.
5. QoS data frame via the netdev path.
6. Read `monitor_probe_tx_status` and `mgmt_tx_compare_status`.

v12 commit: `d50014ca669c wifi: add unrestricted monitor mode injection experiment`.

```text
Bouquet-5.10.258-Bouquet-v4.9-marble-monitor-raw-inject-v12.zip
SHA-256 176d103a581d5d32159e43ff2f33e9dd03a61e36e82ffefeac5afb956833d4c1
```

v12 payload verification (2026-08-24):

- Image SHA-1 2484ae66a96c92e376ec9eda252c8363cab4377b (camera-safe v3)
- camera.ko SHA-256 1cb0e9db7b9133904e904a131dd998f5993979a0bf6e492856e6bdcbaf9036eb
- cfg80211.ko SHA-256 618b5e4c1cb79ada479e1a55b1c7eccb236e42248fb15fe4bdaafa2261a10a2a
- qca `__versions` SHA-256 e0e27f32aaa5c909ae1c06a7f46815df6e7756d14da18830ac67707c007d5f32

Test tools (static aarch64): `/tmp/kilo/mon_inject_raw`,
`/tmp/kilo/nl80211_mgmt_tx`, script `/tmp/kilo/monitor_raw_inject_test.sh`.

### Step H runtime results (v13-v15)

True monitor conparam (`con_mode=4` + wifi off) makes
`monitor_mode_channel` work (`mon_chan_set_stage=20`); the mission-mode
STA+MON policy gate was the earlier sysfs EINVAL.

Monitor vdev findings:

- nl80211 probe on the monitor vdev queues and the firmware completes
  with `DISCARD`; WMI params are identical to the successful v11 STA
  record except the vdev type (`freq=0`, self peer, `tid=0:0`,
  `use6=0`, `flags=00000000`).
- `monitor_mgmt_tx_chanfreq=5180` (explicit) also `DISCARD`.
- probe, deauth, and QoS data via the raw netdev path all `DISCARD`.
- Conclusion: this production firmware rejects `WMI_MGMT_TX_SEND` on
  monitor-type vdevs regardless of frame type or channel field.

Path fixes landed:

- v14 `2a1533e60ef4`: monitor stypes `.tx = 0xffff`, TX queue/carrier
  start for the monitor netdev (qdisc previously parked injected
  frames), `monitor_mgmt_tx_chanfreq`, `mon_xmit_stage` telemetry.
- Raw netdev injection on the monitor vdev now reaches the firmware
  (`mon_xmit_stage=4`, `queued=1`) — firmware still discards.
- cfg80211 requires `SA == wdev address` for non-public-action mgmt
  frames (mlme.c SA check), so foreign-SA frames are only possible via
  the raw netdev path, which the monitor firmware discards.

Viable route:

- v15 `2e743cecd15f`: `monitor_mgmt_tx_unrestricted` now also applies
  to the connected STA vdev — any management subtype through
  `NL80211_CMD_FRAME` on `wlan0` on the exact home channel. The STA
  vdev mgmt engine is firmware-validated (v11 `COMPLETE_OK`).

v15 runtime results, 2026-08-24 (connected to VIVOFIBRA-2250-5G, 5200 MHz):
```text
auth   (fc=b0, own SA, DA=AP):  COMPLETE_OK status=0
deauth (fc=c0, own SA, DA=AP):  COMPLETE_OK status=0
probe  (fc=40, own SA):         COMPLETE_OK status=0
queued=3 completed=3 complete_ok=3 discard=0
```

Over-air proof: after the injected deauth the AP dropped the client and
the phone reconnected with a fresh MAC address.

Constraints:

- cfg80211 requires `SA == wdev address` for non-public-action
  management frames, so foreign-SA frames are blocked at cfg80211 on
  the nl80211 route; the raw monitor netdev route accepts them but the
  monitor-vdev firmware discards.
- Data frames cannot use these routes: cfg80211 rejects non-management
  frames on `NL80211_CMD_FRAME`, and the monitor vdev firmware discards
  them.

### Step I — source-MAC spoofing experiment (v16/v17)

The one purely host-side wall was tested by bypassing cfg80211 with a
direct trigger instead of modifying the shared cfg80211 module:

- v16 `7507da9520d2`: `monitor_mgmt_tx_spoof_sa` gate (skips the driver
  SA rewrite) and `monitor_spoof_tx` root-only trigger
  (`freq:hex-frame`) that submits directly to the STA-vdev management
  TX path.
- v17 `48720fa632a0`: trailing-whitespace tolerance in the trigger
  parser.

Runtime result, 2026-08-25 (connected to PRINT POST 5G, 5745 MHz):

```text
own-SA probe (control):        COMPLETE_OK
foreign-SA probe (02:00:...):  COMPLETE_OK
foreign-SA deauth to AP:       COMPLETE_OK
queued=4 completed=4 complete_ok=4 discard=0
```

Conclusion: **the QCA6490 firmware does not enforce the source address
on the STA-vdev management-TX path.** MAC spoofing on management frames
is possible once the host-side cfg80211 SA check is bypassed.

Real-world validation: the phone stayed connected after the foreign-SA
deauth — the AP correctly rejected the deauth from an unassociated
source address, so client-impersonation toward a well-behaved AP does
not disconnect the station. AP-impersonation frames toward other
clients remain within the same capability envelope.

### Step D — firmware dbglog observer (only if comparison is inconclusive)

Add read-only QCA telemetry around the existing dbglog receive path, not a
firmware modification:

1. Count invocations of `dbglog_parse_debug_logs()`.
2. Count raw WMI debug records after the `dropped` word is removed.
3. Record the last received debug-record header, module id, debug id, and
   number of arguments.
4. Record whether `cds_is_multicast_logging()` was true when a record arrived.
5. Expose those counters read-only beside `monitor_probe_tx_status`.

This separates the remaining possibilities without guessing:

- zero callback invocations: firmware sent no debug events;
- callback activity but no multicast record: host sink/configuration blocked it;
- decoded WAL/MGMT_TXRX records: use section 5 to identify the discard branch.

Only after that observer confirms events are absent should static firmware
analysis take priority over live tracing.

### Step E — decode raw DBR records (fallback)

If firmware debug logs are captured as raw DBR records, decode with
`fw-api/fw/dbglog.h` bit layout:

```text
record header word:
  bits 26-31: num args
  bits 18-25: vdev id
  bits 10-17: module id
  bits  0- 9: debug id
followed by: timestamp word + args
```

Then look up the format string in `Data20.msc` using the module id + debug id.
The `.msc` first column is the catalog key; the module column is implied by
the firmware module table (WAL entries 903-932, mgmt_txrx entries 3699-3715
for `amss20.bin`).

### Step F — deep static analysis (Ghidra), if logs are inconclusive

Setup already done and working:

- Ghidra 12.1.3: `/tmp/kilo/ghidra_12.1.3_PUBLIC/`
- JDK 23: `/tmp/kilo/jdk23/`
- `launch.properties` already points at the JDK.

Import command used:

```bash
support/analyzeHeadless /tmp/kilo/ghidra-proj amss20 \
  -import /tmp/kilo/fw-qca6490/amss20.bin \
  -processor ARM:LE:32:v7 -cspec default \
  -scriptPath /tmp/kilo/ghidra-scripts \
  -postScript FindMgmtRefs.class
```

Known-good anchors for cross-references (section 4 table):

- 0x15d6b10, 0x19245d0 (command tag 0x1a8 descriptors)
- 0x191ef70, 0x192792c (event tag 0x1a9 descriptors)
- 0x15e2044 (event-id table entry 0x128001A9)

Next steps in Ghidra: find which functions reference the 0x1a9 descriptor
tables (the completion-event builder), then find the code that stores
`status = 1` (DISCARD) into the completion fixed param before calling it, and
walk backwards to the condition. Also load `/tmp/kilo/fw-qca6490/m3.payload.bin`
as `ARM:LE:32:v7` (Thumb) at base 0 and resolve the `"bad descr_idx"` string
reference.

### Step G — fix direction once the discard cause is confirmed

Kernel-side only, keeping the experiment bounded:

1. If TID/enqueue discard: rework the monitor probe TX path so the frame is
   sent on a vdev/queue the firmware accepts (e.g., align TID and
   `wmi_mgmt_params.tx_param` fields with the P2P off-channel pattern in
   `wlan_p2p_off_chan_tx.c:1124-1234`, which is the firmware-tested path).
2. If channel discard: ensure `chanfreq` matches the vdev's current firmware
   channel state (possibly require an explicit vdev-start on the target
   channel instead of the monitor-mode channel sysfs only).
3. If peer/pdev mismatch: register/resolve the peer the WAL expects for local
   mgmt TX.
4. If M3 desc rejection: fix the desc_id/cookie passed down to the M3 path.

After any fix: rebuild ONLY `qca_cld3_qca6490.ko`, re-verify its `__versions`
checksum against the camera-safe v3 Image ABI
(`e0e27f32aaa5c909ae1c06a7f46815df6e7756d14da18830ac67707c007d5f32` for
qca, `515f49ce8cabdcac6cea16aef7395b5d9dee95468c5fac275e37cb5d1d55040c`
for cfg80211), and package with the exact v3 Image
(SHA-1 `2484ae66a96c92e376ec9eda252c8363cab4377b`).

## 8. Safety and packaging rules (non-negotiable)

- The probe TX gate stays default-off (`monitor_probe_tx` module param).
- Only broadcast probe requests are allowed by the kernel patch; no other
  frame types.
- Preserve the exact camera-safe v3 Image. Never ship a freshly rebuilt whole
  Image without a camera regression check:
  - bad signature to avoid: `Invalid kmd buf size` in `camera.ko`
    (commit `5e0e1c9664a9` removes it; the camera-safe module is the one from
    `marble-bouquet-4.9-csi-r2`, SHA-256
    `1cb0e9db7b9133904e904a131dd998f5993979a0bf6e492856e6bdcbaf9036eb`).
- One small change per flashable package; verify `7z t`, Image SHA-1, module
  `__versions` checksums, and `kernel.release` before flashing.
- All source changes must be committed in the `wifi/monitor-mode` branch of
  `/home/ubuntu/kernel-compile/default-v2/WIFI-MONITOR-EXPERIMENT`.

## 9. Artifacts

- Firmware: `/tmp/kilo/fw-qca6490/` (+ `m3.payload.bin`)
- WEXT client: `/tmp/kilo/wlan_priv` (source `/tmp/kilo/wlan_priv.c`)
- Direct firmware-log listener: `/tmp/kilo/cld_fwlog_watch`
  (source `/tmp/kilo/cld_fwlog_watch.c`)
- Ghidra: `/tmp/kilo/ghidra_12.1.3_PUBLIC/`, scripts `/tmp/kilo/ghidra-scripts/`
- Current STA home-channel experiment ZIP:
  `Bouquet-5.10.258-Bouquet-v4.9-marble-monitor-probe-tx-v11-sta-home-channel.zip`
  SHA-256 `1131dc9b164231bc56266a2d05dc8ff1593fd8400368cc8b182a1d16acded70f`
