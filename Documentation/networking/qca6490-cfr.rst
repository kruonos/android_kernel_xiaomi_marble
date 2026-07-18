.. SPDX-License-Identifier: GPL-2.0

======================================
QCA6490 CFR and CSI research transport
======================================

Purpose and scope
=================

This implementation provides a bounded, loss-observable transport for
QCA6490 Channel Frequency Response data. The firmware and WLAN driver call
the captured radio data CFR. Research software commonly treats the resulting
complex samples as Channel State Information after applying the required
format decoding, calibration, and lane interpretation.

The kernel transport does not assign physical antenna identities, calibrate
phase, derive channel matrices, or define a signal-processing model. It
preserves capture evidence and metadata so those decisions can be made and
reproduced in userspace.

The implementation has four main parts:

1. An ordered relayfs writer that publishes a complete frame only after all
   frame bytes are resident.
2. The CFRR little-endian framing ABI and bounded per-pdev state.
3. QCA6490 DBR, RX PPDU, correlation, and continuous-recovery handling.
4. Root-only sysfs control plus bounded sysfs and debugfs diagnostics.

Build prerequisites
===================

The Marble defconfig in this branch enables ``CONFIG_RELAY=y`` to match the
main patched kernel. The existing WLAN profile derives streamfs support from
the kernel relay and debugfs settings. A build using this data plane must end
with the following settings::

  CONFIG_RELAY=y
  CONFIG_WLAN_CFR_ENABLE=y
  CONFIG_WLAN_ENH_CFR_ENABLE=y
  CONFIG_WLAN_STREAMFS=y

The QCA6490 target, direct buffer receive support, and the fixed CFR folder
path must also be enabled by the normal WLAN target configuration. Qualcomm
vendor builds commonly translate the ``CONFIG_WLAN_*`` settings into these
compiler definitions::

  WLAN_CFR_ENABLE
  WLAN_ENH_CFR_ENABLE
  WLAN_STREAMFS
  CFR_USE_FIXED_FOLDER
  QCA_WIFI_QCA6490

If ``CONFIG_RELAY`` or ``WLAN_STREAMFS`` is absent, the QDF streamfs helpers
compile as stubs and no relay channel is created. This can make a source build
pass while leaving the runtime transport unavailable. Check the final kernel
configuration and WLAN compile commands, not only the source defconfig.

Transport architecture
======================

The primary data path is one global relay channel per CFR pdev::

  firmware CFR event
          |
          +--> DBR callback --> RAW_DBR + DBR_META records
          |
          +--> RX PPDU callback --> RX_PPDU record
          |
          +--> bounded correlation --> FINAL record when matched
          |
          +--> relayfs CFRR stream --> userspace recorder

Raw DBR and RX PPDU evidence is written before correlation updates the lookup
table. A failed or delayed correlation therefore does not erase the raw input
needed for later analysis.

Every high-rate write follows these rules:

* Writers are serialized by the per-pdev CFR record lock.
* A complete frame is assembled in a fixed 32 KiB staging buffer.
* The ordered writer pins the current CPU for relay cursor stability but leaves
  local interrupts enabled during the copy.
* Hard-IRQ writers are rejected. CFR writers are supported from process,
  softirq, and NAPI contexts under the per-pdev serializer.
* The relay writer copies the complete frame before release-publishing the
  new relay offset.
* The normal relay read path acquire-loads the offset before copying bytes to
  userspace.
* A full relay drops the complete new frame. It never publishes a partial
  frame.
* The sequence number is allocated before reservation, so a dropped frame is
  visible as a sequence gap.

Optional vendor netlink duplication is independent from relayfs. It uses a
separate fixed staging buffer and is not required for CFRR recording. Relayfs
is preferred for sustained capture because netlink requires skb allocation
and a second payload copy.

Fixed memory
============

The per-pdev fixed transport allocation is:

=========================  ==========
Component                  Bytes
=========================  ==========
Relay ring, 32 KiB x 128     4,194,304
Relay staging buffer            32,768
Optional netlink staging         32,768
Total fixed transport         4,259,840
=========================  ==========

Correlation history and counters are also bounded. The implementation does
not add a variable-size event queue and does not allocate in the RX PPDU or
DBR mirror path.

CFRR framing ABI
================

The CFRR frame header and session records use explicit little-endian
declarations. The copied main-kernel DBR metadata and RX PPDU evidence payloads
use fixed-width integer declarations on the little-endian Marble target. Packed
transport structure sizes are checked at build time.

The version 2 frame header is 64 bytes:

======  =====  ======================================
Offset  Size   Field
======  =====  ======================================
0       4      magic, ``0x52524643``
4       2      framing version
6       2      header length
8       4      record type
12      4      flags
16      4      sequence number
20      4      payload length
24      4      type-specific ``meta0``
28      4      type-specific ``meta1``
32      8      monotonic timestamp in nanoseconds
40      8      session identifier
48      4      pdev identifier
52      4      reserved
56      8      reserved
======  =====  ======================================

The complete frame length is ``header length + payload length`` and must not
exceed 32 KiB. Reserved fields must be zero when written and ignored when
read. Readers should use ``header length`` rather than assuming that every
future compatible header remains 64 bytes.

Record types
------------

=================  ============  ===========================================
Name               Value         Payload
=================  ============  ===========================================
FINAL              0x00000000    Legacy correlated CFR header and data
RAW_DBR            0x80000000    Validated raw DBR bytes
RX_PPDU             0x80000001    Fixed RX PPDU evidence snapshot
DBR_META            0x80000002    Parsed DBR metadata version 1
SESSION_START       0x80000003    Session configuration and relay geometry
SESSION_END         0x80000004    Final counters and stop reason
REARM               0x80000005    Recovery stage, status, epoch, and timing
=================  ============  ===========================================

``RAW_DBR`` uses the DBR cookie in ``meta0`` and the validated byte length in
``meta1``. ``RX_PPDU`` uses the PPDU identifier in ``meta0`` and the SRNG
identifier in ``meta1``. ``DBR_META`` uses the DBR cookie and PPDU identifier.
``REARM`` uses the recovery stage and low 32 bits of the recovery epoch.

Session payload sizes are:

=========================  =====
Payload                    Bytes
=========================  =====
Session start version 1       64
Session start version 2       80
Session end version 1         80
Rearm version 1               60
=========================  =====

Session start version 2 retains the complete version 1 prefix and appends
capture count, interval mode, continuous-recovery state, and watchdog stall
time. Readers that understand only the version 1 prefix can still inspect the
base configuration.

The fixed DBR metadata prefix is 112 bytes. Optional validated raw DMA header
bytes follow that prefix in the same DBR_META payload. The RX PPDU evidence
payload is a fixed 40-byte structure containing ten 32-bit fields emitted by the
little-endian target.

Reader requirements
-------------------

A research recorder should:

1. Use a bounded input buffer.
2. Search for CFRR magic only when synchronization has been lost.
3. Validate version, header length, payload length, and total frame length.
4. Wait for the rest of an incomplete frame instead of exporting it.
5. Track sequence numbers within each session identifier.
6. Treat unknown record types and compatible future versions as opaque.
7. Preserve every valid FINAL record and every unmatched RAW_DBR record.
8. Record sequence gaps, resynchronization bytes, invalid frames, file size,
   and a cryptographic file hash in capture metadata.

There is no per-frame checksum in CFRR. File-level hashing is recommended for
archival research data. A duplicated or out-of-order old sequence must not
reset the high-water mark and amplify one anomaly into a large false gap.

Session lifecycle
=================

Enabling relay output creates the relay channel but does not create an idle
capture session. A session starts only after capture configuration has been
committed and capture is marked active.

The normal lifecycle is:

1. Configure capture and watchdog parameters.
2. Reset the relay before a new experiment.
3. Open the relay reader.
4. Start capture, which emits one SESSION_START record.
5. Record raw, metadata, PPDU, final, and recovery records.
6. Stop capture, which emits one SESSION_END record.
7. Close the reader after SESSION_END.

If the relay is already full, SESSION_END can be dropped like any other
complete record. Kernel counters then show a session error and a session
record drop. This is expected overload behavior, not a partial-frame failure.

Continuous-capture recovery
===========================

Some QCA6490 firmware revisions stop delivering DBR data after a finite burst
even while RX PPDU traffic continues. The recovery worker uses real PPDU and
DBR timestamps as liveness evidence. It does not synthesize startup evidence
and it does not run firmware commands from callback context.

The stages are:

``soft``
  Re-submit the committed RCC configuration.

``hard``
  Disable RCC, wait for a bounded drain interval, pause CFR datapath and
  monitor reap processing, release held DBR buffers, clear bounded correlation
  state, restore datapath processing, and re-enable RCC.

``blind``
  Run one additional hard transaction if a successful regular hard recovery
  produces neither new PPDU nor new DBR evidence within the stall interval.

A blind transaction never arms another blind transaction. This rule prevents
an unattended reset loop when radio traffic is absent or firmware recovery is
not possible.

The default watchdog values are:

================  ======
Parameter         Value
================  ======
Poll interval     50 ms
Stall threshold   250 ms
Drain interval    75 ms
Rearm backoff     500 ms
================  ======

The worker runs in process context. Firmware transactions are serialized by a
dedicated mutex. Configuration and lifecycle operations use separate locks
and a generation counter so stop or reconfiguration can cancel stale work.
The relay file descriptor, session identifier, sequence, fixed buffers, and
correlation allocation remain valid across in-session recovery.

Control interface
=================

The Marble QCA6490 build exposes the root-only control file at::

  /sys/kernel/qca6490/cfr_control

The complete scalar status is available at::

  /sys/kernel/qca6490/cfr_status

The control file accepts:

* Capture commands: ``direct_ftm``, ``all_ftm_ack``, ``direct_ndpa``,
  ``all_ndpa``, ``all_packet``, ``continuous_capture``, and ``stop``.
* Relay commands: ``relay_on``, ``relay_off``, and ``relay_reset``.
* Counter commands: ``clear`` and
  ``reader_stats <gaps> <resync_bytes> <invalid>``.
* Capture configuration: ``window``, ``duration``, ``interval``, ``count``,
  ``interval_mode``, and ``bitmap``.
* Recovery configuration: ``continuous on|off`` and
  ``watchdog <stall_ms> <poll_ms> <drain_ms>``.
* Profiles: ``profile_default``, ``profile_aggressive``, and
  ``profile_continuous``.

``direct_ftm`` is the lowest-risk starting mode. ``continuous_capture`` uses
the all-packet filter and can impose significant WLAN and CPU load.

The firmware capability must be checked before selecting count mode::

  cat /sys/kernel/qca6490/cfr_capture_count_supported

On the tested QCA6490 firmware this value was zero. That firmware requires
duration mode plus host recovery. ``profile_continuous`` reads the firmware
capability and automatically selects duration mode when count mode is not
supported.

Example duration-mode experiment
--------------------------------

The reader must be opened before capture starts. The following commands show
the kernel side of a 100 ms capture experiment::

  mount -t debugfs none /sys/kernel/debug
  printf 'stop\n' > /sys/kernel/qca6490/cfr_control
  printf 'relay_reset\n' > /sys/kernel/qca6490/cfr_control
  printf 'clear\n' > /sys/kernel/qca6490/cfr_control
  printf 'relay_on\n' > /sys/kernel/qca6490/cfr_control
  printf 'window 100000 100000\n' > /sys/kernel/qca6490/cfr_control
  printf 'interval_mode duration\n' > /sys/kernel/qca6490/cfr_control
  printf 'continuous on\n' > /sys/kernel/qca6490/cfr_control
  printf 'watchdog 250 50 75\n' > /sys/kernel/qca6490/cfr_control

Open and continuously drain this relay file in a separate process::

  /sys/kernel/debug/cfrwlan0/cfr_dump0

Then start and stop capture::

  printf 'continuous_capture\n' > /sys/kernel/qca6490/cfr_control
  printf 'stop\n' > /sys/kernel/qca6490/cfr_control

The recorder should continue until SESSION_END. A byte limit can terminate the
reader before the kernel session ends and should not be used for acceptance
captures unless rotation is implemented.

Diagnostics
===========

Detailed diagnostics use debugfs seq_file readers::

  /sys/kernel/debug/cfrwlan0/status
  /sys/kernel/debug/cfrwlan0/relay_stats
  /sys/kernel/debug/cfrwlan0/session
  /sys/kernel/debug/cfrwlan0/correlation
  /sys/kernel/debug/cfrwlan0/continuous

Important relay checks are:

* ``frames_attempted`` equals ``frames_committed + frames_dropped``.
* ``reservation_failures`` equals full-relay drops.
* ``invalid_internal_lengths`` remains zero.
* Reader-reported gaps match expected sequence gaps.
* ``reader_invalid_frames`` and ``reader_resync_bytes`` remain zero during a
  healthy capture.

Important recovery checks are:

* Hard recovery status fields remain zero.
* ``lut_reset_failures`` and ``dp_cycle_failures`` remain zero.
* Every blind record is followed by real PPDU or DBR evidence.
* ``blind_retry_pending`` returns to zero after recovery or stop.

Important teardown checks are:

* capture and continuous activity are zero.
* relay output is disabled when requested.
* PPDU subscription is removed.
* pending DBR and RX entries are zero.

Overload behavior
=================

With no reader, the relay eventually fills and rejects complete new records.
The implementation keeps recording attempted sequences and per-type drops.
In a controlled no-reader test, 21,242 frames were attempted, 3,348 were
committed, and 17,894 were dropped. Reservation failures matched drops and no
malformed record was reported.

This test demonstrates transport behavior under pressure. It does not imply
that dropping research samples is acceptable. Normal experiments should use a
reader that drains the relay continuously and reports any sequence gap.

Validation evidence
===================

The source-linked golden ABI check validates exported field declarations, fixed
packed sizes, and byte-exact vectors from the copied main-kernel ABI::

  python3 tools/testing/selftests/net/qca6490_cfr_abi.py

The implementation was exercised on a POCO F5 Marble device with QCA6490 WLAN
and firmware capture-count support disabled.

An uncapped 75 second run recorded:

* 34,132 complete CFRR records.
* 14,149 raw DBR records and 14,149 DBR metadata records.
* 3,947 RX PPDU records.
* 1,764 final records with byte-exact raw matches.
* 60 soft, 60 hard, and one blind recovery record.
* Zero sequence gaps, malformed records, relay drops, LUT reset failures,
  datapath cycle failures, or recovery failures.

The blind recovery restored raw DBR in 3.934 ms and RX PPDU in 13.195 ms.

An uncapped 180 second run recorded:

* 72,599 complete CFRR records and 79,875,376 bytes.
* 28,729 raw DBR records and 28,729 DBR metadata records.
* 11,413 RX PPDU records.
* 3,370 final records with byte-exact raw matches.
* 175 soft, 175 hard, and six blind recovery records.
* Zero sequence gaps, sequence resets, out-of-order records, malformed
  records, kernel drops, relay overruns, datapath cycle failures, LUT reset
  failures, or recovery failures.

All six blind transactions restored raw DBR and RX PPDU. Raw recovery latency
was 3.896 to 72.941 ms. RX PPDU recovery latency was 13.342 to 119.757 ms.

Performance impact
==================

Continuous all-packet capture is not free. In one controlled 400-packet ICMP
comparison at a 50 ms interval:

=========================  ============  ============
Metric                     CFR disabled  CFR active
=========================  ============  ============
Packet loss                0 percent     0.5 percent
Average RTT                23.242 ms     55.695 ms
Maximum RTT                170.313 ms    623.228 ms
Whole-device CPU busy      6.31 percent  29.02 percent
Recorder process CPU       n/a           1.38 percent
=========================  ============  ============

Most measured cost was in the kernel, WLAN datapath, firmware interaction, and
radio workload rather than the recorder process. Results depend on traffic,
firmware, power state, and device temperature. Performance measurements should
be repeated for each research workload.

Security and research ethics
============================

CFR and CSI can reveal properties of radio traffic and the surrounding
environment. Control is root-only, but debugfs relay access must also be
restricted by the device environment. Captures should be collected only on
networks and devices for which the operator has authorization.

Raw capture files can contain stable environmental signatures. Treat them as
sensitive research data. Store capture metadata, kernel identity, module hash,
configuration, timestamps, and file hashes with every experiment.

Known limitations
=================

* The tested QCA6490 firmware does not advertise capture-count mode.
* The 32 KiB maximum record size is selected for the Marble QCA6490 target and
  is not a generic maximum for larger Qualcomm CFR targets.
* Physical lane-to-antenna mapping and RF calibration are outside the kernel
  transport.
* A full relay can drop SESSION_END and leave the session state as error.
* Optional netlink duplication performs allocation and copying in an atomic
  context and is not recommended for sustained high-rate capture.
* All-packet capture has measurable latency and CPU impact.
* The ordered relay copy can span up to 32 KiB with preemption disabled. Local
  interrupts remain enabled, but scheduler latency should still be measured
  for workloads that approach the maximum record size.
* Recovery status zero means the command path succeeded. Userspace should also
  confirm that PPDU or DBR evidence resumed.

Source map
==========

==============================  ================================================================
Area                            Primary source
==============================  ================================================================
CFRR ABI and recovery bounds    ``umac/cfr/core/inc/cfr_defs_i.h``
Per-pdev bounded state          ``umac/cfr/dispatcher/inc/wlan_cfr_utils_api.h``
Relay and recovery engine       ``umac/cfr/core/src/cfr_common.c``
Final capture fan-out           ``umac/cfr/dispatcher/src/wlan_cfr_tgt_api.c``
UCFG transaction boundary       ``umac/cfr/dispatcher/src/wlan_cfr_ucfg_api.c``
QCA6490 WDI and datapath         ``target_if/cfr/src/target_if_cfr_6490.c``
DBR, PPDU, and correlation      ``target_if/cfr/src/target_if_cfr_enh.c``
Sysfs control and health        ``qcacld-3.0/core/hdd/src/wlan_hdd_cfr.c``
Ordered QDF relay writer        ``qdf/linux/src/qdf_streamfs.c``
Relay read publication pairing  ``kernel/relay.c``
CFRR golden ABI check           ``tools/testing/selftests/net/qca6490_cfr_abi.py``
==============================  ================================================================
