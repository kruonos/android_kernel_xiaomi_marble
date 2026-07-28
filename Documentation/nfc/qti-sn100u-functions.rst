SN100U named function surface
=============================

This list is generated from ``qti_nfc_function_id``.  It contains
stable catalog names and their implementation binding. ``direct`` calls
have kernel-defined semantics, ``built-in`` calls use a fixed request
implemented by the gateway, and ``payload`` calls require a
firmware-generation-specific backend payload supplied by privileged
userspace. Payload entries are catalogued transport aliases, not claims
that the kernel implements their high-level vendor semantics.

Controller lifecycle and transport
----------------------------------

* ``get-controller-info`` — direct
* ``get-runtime-state`` — direct
* ``power-on`` — direct
* ``power-off`` — direct
* ``power-cycle`` — direct
* ``hard-reset`` — direct
* ``iso-reset`` — payload
* ``enable`` — direct
* ``disable`` — direct
* ``core-reset-init`` — built-in
* ``pre-discover`` — payload
* ``request-control`` — payload
* ``release-control`` — payload
* ``factory-reset`` — payload
* ``receive-frame`` — direct
* ``ldo-enable`` — direct
* ``ldo-disable`` — direct
* ``get-platform-type`` — direct
* ``get-irq-state`` — direct

Firmware, download, recovery, and firmware diagnostics
------------------------------------------------------

* ``enter-download-mode`` — direct
* ``exit-download-mode`` — direct
* ``get-download-mode`` — direct
* ``get-firmware-version`` — built-in
* ``get-download-session`` — built-in
* ``download-reset`` — payload
* ``download-start`` — payload
* ``download-write`` — payload
* ``download-complete`` — payload
* ``download-check-integrity`` — built-in
* ``download-force`` — payload
* ``download-recover`` — payload
* ``download-emergency-recover`` — payload
* ``download-get-status`` — payload
* ``download-set-status`` — payload
* ``download-get-log`` — payload
* ``download-read-log`` — payload
* ``download-send-nci`` — payload
* ``check-firmware-version`` — payload
* ``check-flash-required`` — payload
* ``check-torn-session`` — payload
* ``configure-lx-debug`` — payload

RF, discovery, tag, and vendor configuration
--------------------------------------------

* ``start-discovery`` — payload
* ``stop-discovery`` — payload
* ``tag-transceive`` — payload
* ``notify-polling-frame`` — payload
* ``set-forum-mode`` — payload
* ``set-mifare-reader`` — payload
* ``update-rf-misc`` — payload
* ``update-rf-config`` — payload
* ``get-rf-config`` — payload
* ``set-rssi-config`` — payload
* ``set-extended-field-mode`` — payload
* ``get-rf-field-state`` — direct
* ``set-lpcd`` — payload
* ``get-ulp-detection`` — payload
* ``set-ulp-detection`` — payload
* ``get-observe-support`` — payload
* ``get-observe-mode`` — payload
* ``set-observe-mode`` — payload
* ``set-observe-tech`` — payload
* ``set-autonomous-mode`` — payload
* ``update-autonomous-power`` — payload
* ``set-guard-timer`` — payload
* ``set-srd-timeout`` — payload
* ``apply-clock-config`` — payload
* ``get-clock-config`` — payload
* ``set-dcdc-config`` — payload
* ``get-gpio-status`` — direct
* ``set-gpio`` — direct
* ``set-discovery-shutdown`` — payload
* ``write-sram-config-to-flash`` — payload
* ``get-vendor-capabilities`` — payload
* ``get-vendor-parameter`` — payload
* ``set-vendor-parameter`` — payload
* ``get-verbose-logging`` — payload
* ``set-verbose-logging`` — payload
* ``power-tracker-start`` — payload
* ``power-tracker-stop`` — payload
* ``power-tracker-read`` — payload
* ``set-transit-config`` — payload
* ``set-china-transit`` — payload

Routing and NFCEE
-----------------

* ``get-routing`` — direct
* ``set-routing`` — payload
* ``clear-routing`` — payload
* ``commit-routing`` — payload
* ``add-aid-routing`` — payload
* ``remove-aid-routing`` — payload
* ``set-default-tech-routing`` — payload
* ``set-default-protocol-routing`` — payload
* ``nfcee-register`` — payload
* ``nfcee-discover`` — payload
* ``nfcee-get-info`` — payload
* ``nfcee-get-active`` — payload
* ``t4t-open`` — payload
* ``t4t-close`` — payload
* ``t4t-select-application`` — payload
* ``t4t-select-file`` — payload
* ``t4t-read-file`` — payload
* ``t4t-update-file`` — payload
* ``t4t-clear`` — payload

eSE, UICC, wired SE, APDU, and JCOP
-----------------------------------

* ``ese-power-on`` — direct
* ``ese-power-off`` — direct
* ``ese-get-power`` — direct
* ``ese-reset`` — payload
* ``ese-cold-reset`` — payload
* ``ese-cold-reset-protect`` — payload
* ``se-get-atr`` — payload
* ``se-is-present`` — payload
* ``se-transmit-apdu`` — payload
* ``se-open-basic-channel`` — payload
* ``se-open-logical-channel`` — payload
* ``se-close-channel`` — payload
* ``wired-se-start`` — payload
* ``wired-se-dispatch`` — payload
* ``uicc-save-parameters`` — payload
* ``uicc-restore-parameters`` — payload
* ``uicc-get-hci-parameters`` — payload
* ``uicc-set-hci-parameters`` — payload
* ``uicc2-enable-swp`` — payload
* ``ese-power-manager-on`` — payload
* ``ese-power-manager-off`` — payload
* ``ese-power-manager-state`` — payload
* ``ese-power-manager-kill-all`` — payload
* ``ese-power-manager-ioctl`` — payload
* ``jcop-initialize`` — payload
* ``jcop-check-version`` — payload
* ``jcop-start-download`` — payload
* ``jcop-get-state`` — payload
* ``jcop-set-state`` — payload
* ``jcop-reset-update`` — payload
* ``jcop-deinitialize`` — payload

Diagnostics, test, charging, and manufacturing
----------------------------------------------

* ``trace-get-info`` — direct
* ``trace-read`` — direct
* ``trace-clear`` — direct
* ``trace-set-capture`` — direct
* ``trace-set-max-length`` — direct
* ``trace-set-dmesg`` — direct
* ``dta-get-config`` — payload
* ``dta-set-config`` — payload
* ``dta-parse-config`` — payload
* ``wlc-enable`` — payload
* ``wlc-start`` — payload
* ``wlc-start-power-transfer`` — payload
* ``get-system-property`` — payload
* ``set-system-property`` — payload

Complete transport fallback
---------------------------

* ``nci-transaction`` — payload
* ``fw-download-transaction`` — payload
* ``write-only`` — payload
* ``read-only`` — payload
