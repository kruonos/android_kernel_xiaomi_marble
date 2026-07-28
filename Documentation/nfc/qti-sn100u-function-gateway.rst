.. SPDX-License-Identifier: GPL-2.0

QTI SN100U named function gateway
=================================

Purpose
-------

The Marble QTI NFC driver is a transport driver.  NCI, proprietary NXP
functions, NFCEE functions, and firmware-download orchestration normally live
in Android userspace.  The function gateway adds a versioned research ABI to
``/dev/nq-nci`` so a single privileged owner can invoke catalogued functions
without making packet opcodes part of the public catalog.

The gateway does not replace the stock Android NFC HAL.  A gateway session and
the HAL are mutually exclusive because both consume responses and asynchronous
notifications from the same SN100U transport.

ABI
---

The UAPI is defined in
``include/uapi/linux/nfc/qti_nfc_function.h``.  It provides four ioctls:

``QTI_NFC_FUNCTION_GET_ABI``
  Return version, capabilities, and transfer limits.

``QTI_NFC_FUNCTION_SESSION_OPEN``
  Claim exclusive function-gateway ownership.  The call succeeds only when the
  caller is the sole ``/dev/nq-nci`` opener.

``QTI_NFC_FUNCTION_CALL``
  Invoke a named function with a selected kernel, NCI, or firmware-download
  backend.  The fixed-width structure is compatible with 32-bit and 64-bit
  callers and carries up to 4096 bytes of structured function data.  Physical
  NCI/download frames remain bounded to 554 bytes.

``QTI_NFC_FUNCTION_SESSION_CLOSE``
  Release the session and allow the Android HAL to reopen the transport.

Named calls
-----------

Every call contains a stable function identifier, transport selection, timeout,
request bytes, and response storage.  Generated documentation labels each entry
as ``direct``, ``built-in``, or ``payload``.  Direct calls have kernel-defined
semantics.  Built-in calls use a fixed request implemented by the gateway.
Payload calls require a firmware-generation-specific framed request from
privileged userspace and are transport aliases, not kernel implementations of
the named high-level vendor behavior.  The four explicit transport-fallback IDs
remain available for research that has no stable catalog entry.

The exhaustive generated function list is in
:doc:`qti-sn100u-functions`.  It includes controller lifecycle, firmware and
recovery, RF/discovery, routing/NFCEE, eSE/UICC/JCOP, diagnostics, DTA, WLC,
factory functions, and complete transport fallback.

Ownership and lifecycle
-----------------------

A valid sequence is:

1. Open ``/dev/nq-nci`` once.
2. Query ``QTI_NFC_FUNCTION_GET_ABI``.
3. Open a function session.
4. Invoke one or more named calls.
5. Close the function session.
6. Close the device before restarting the Android NFC HAL.

Opening a second raw NFC descriptor or acquiring the legacy diagnostic owner is
blocked while the function session is active.  Closing the owner file releases
the function session automatically and lowers FIRM.  Explicit session close also
restores NCI mode unless ``QTI_NFC_FUNCTION_SESSION_F_KEEP_MODE`` was requested;
the last ``/dev/nq-nci`` file close always restores the transport state.

Firmware download
-----------------

The catalog includes these firmware-download operations; the generated function
list identifies which have fixed gateway requests and which require a
generation-specific backend payload:

* ``enter-download-mode``
* ``get-firmware-version``
* ``get-download-session``
* ``download-reset``
* ``download-start``
* ``download-write``
* ``download-check-integrity``
* ``download-complete``
* ``download-force``
* ``download-recover``
* ``download-emergency-recover``
* ``download-get-log`` and ``download-read-log``
* ``exit-download-mode``

The tool understands NXP firmware container libraries exporting
``gphDnldNfc_DlSequence``.  It reads the exported record stream without
executing the library, validates every big-endian record length, computes the
CRC16/CCITT required by the download transport, and submits each complete frame
through ``download-write``.

Inspect a firmware container without touching hardware::

  nfc_tools/qti-nfc-function inspect-firmware /vendor/lib64/libsn100u_fw.so

Upload its signed record stream::

  nfc_tools/qti-nfc-function upload-firmware \
      /vendor/lib64/libsn100u_fw.so \
      --record /data/local/tmp/sn100u-download.nfcf

The upload command lowers FIRM after completion or failure.  A program
using ``qti_nfc_function_lib`` can instead keep one exclusive session open and
perform manual integrity, logging, or recovery calls before exiting download
mode.  Firmware responses are returned without replacement or emulation;
acceptance, compatibility, and signature decisions are therefore the NFCC's
actual decisions.  Successful record transmission alone is not reported as a
completed installation: the command validates exact session responses and
operational lifecycle state, requires the session to close, compares the
installed version with the image version, and requires the SN100U integrity mask
reported by ``download-check-integrity`` before returning success.

Generic function invocation
---------------------------

List all names and default backends::

  nfc_tools/qti-nfc-function list

Read controller state::

  nfc_tools/qti-nfc-function state

Invoke a named function whose backend schema takes request bytes::

  nfc_tools/qti-nfc-function call set-observe-mode \
      --request-file observe-request.bin \
      --record observe-session.nfcf

The request can also be hexadecimal with ``--request``.  ``--no-response`` is
available for write-only functions; ``--retry-write`` selects the driver's
bounded retry path.

Framed records
--------------

The userspace library can record every session as an ``NFCF`` stream.  Each
little-endian record includes:

* record type;
* session and sequence identifiers;
* named function and selected backend;
* monotonic timestamp;
* operation status;
* payload length and exact request/response payload.

Record types cover session start/end, function calls, results, asynchronous
records, and errors.  This makes firmware experiments reproducible without
placing vendor packet details in the public function catalog.

Tool build
----------

Host build::

  make -C nfc_tools

Android ARM64 build::

  NDK_ROOT=/path/to/android-ndk nfc_tools/build-android.sh

The Android executable is written to
``nfc_tools/out/android-arm64/qti-nfc-function``.
