# Marble Qualcomm 5.15 Port Plan

Date: 2026-09-07

## Goal and Preservation Boundary

Port POCO F5 / Redmi Note 12 Turbo (Marble, SM7475/Ukee) to a genuine
Qualcomm Android 5.15 core while preserving required firmware/userspace
interfaces and eventually restoring the unified Bouquet feature set.

- Repository: https://github.com/kruonos/android_kernel_xiaomi_marble
- Frozen source reference: `unified/all-features-20260907` at
  `cd3f531d2230a22812f6c3f0ffe0148928d70d03`.
- Existing baseline, CSI and MGLRU preservation branches were also pushed
  and their remote hashes verified on 2026-09-07.
- Work branch: `port/waipio-5.15-20260907`, initially based on that unified commit.
- The unified source merge is preserved, but its full build/runtime validation
  is not established. Do not confuse it with the tested 5.10 baseline ZIP.
- Keep the existing known-good ZIP and recovery backup untouched.
- GitHub backup covers tracked source and reachable history, not ignored build
  outputs, local archives, firmware, recovery images or arbitrary workspace files.

## Storage and Execution Rules

- No new local backup archives, duplicate clones or additional worktrees.
- Use the existing checkout and Git object database. Branch creation does not
  duplicate the source tree.
- Inspect remote files before fetching source. Check free space before each
  source fetch or build; new upstream Git objects still consume disk space.
- Fetch only selected refs; do not mirror every vendor branch/tag.
- Acquire external repositories one at a time only after dependencies and
  revisions are identified. Keep them in purpose-named workspace directories.
- Run heavy work serially, with at most four workers and reduced priority.
- Do not delete existing outputs or backups to make room without approval.
- Existing `out/` contains 5.10 artifacts: never incrementally build 5.15 into
  it or treat its modules/configuration as 5.15 outputs. Use a dedicated target
  output directory only after a storage budget is approved.
- No flash or partition writes without explicit authorization.

## Candidate Source Stack

Primary candidate branch, called `P11` below:
`android-msm-p11-5.15-tm-wear-kr3-dr-p11-qpr3-release`.

| Role | Source | Current evidence |
| --- | --- | --- |
| Qualcomm 5.15 core | https://android.googlesource.com/kernel/msm/ | P11 Makefile is 5.15.149; Waipio build config and actual GCC, DisplayCC, interconnect and UFS PHY drivers verified |
| Android Common/build integration | https://android.googlesource.com/kernel/common/ | P11 Waipio config sources sibling `common` configs; exact matching revision/toolchain still required |
| Video DT | https://android.googlesource.com/kernel/msm-extra/video-devicetree/ | P11 contains `ukee-vidc.dts`; EOS Android 14 is not needed for this file |
| Video driver | https://android.googlesource.com/kernel/msm-extra/video-driver/ | P11 `driver/platform/waipio/src/msm_vidc_waipio.c:3331-3335` explicitly handles `qcom,msm-vidc-ukee` |
| Exact board support and feature inventory | This Bouquet tree | Primary local source reference, with tested 5.10 behavior as the control |
| OEM provenance | MiCode Marble kernel, kernel_devicetree, audio and WLAN repositories | Comparison sources, not replacements for Bouquet fixes |
| Additional 5.15 modules | Google Qualcomm WLAN, MMRM, RMNET, audio and other repositories | Candidates supplied in research; complete dependency/branch audit remains outstanding |
| Secondary integration examples | LineageOS Xiaomi SM8550 kernel/modules/devicetrees; CLO msm-5.15 | API/build references, not Marble hardware authorities |

Verified core paths under P11:

- `Makefile:2-4`: 5.15.149.
- `build.config.msm.waipio:8-19`: Waipio, GKI variant, header v3, vendor DLKM.
- `drivers/clk/qcom/gcc-waipio.c:3820`: `qcom,waipio-gcc`.
- `drivers/clk/qcom/dispcc-waipio.c:1971`: `qcom,waipio-dispcc`.
- `drivers/interconnect/qcom/waipio.c:2539-2559`: Waipio fabric matches.
- `drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-waipio.c:304`: Waipio PHY match.

These branch-relative lines are research evidence, not immutable pins. Resolve
commit IDs before implementation. Source presence is not evidence of a tested
Marble build. Identical branch names across repositories do not replace a
matching manifest or dependency validation.

## Phase 1: Pin Sources and Map Gaps

1. Resolve the kernel, common, build/toolchain, platform DT and external-module
   revisions from a release manifest where available. Record explicit pins and
   unresolved relationships; do not mix Wear releases merely by version number.
2. Inspect build inclusion, Kconfig and compatible matches for the entire
   Marble -> Ukee -> Cape/Waipio dependency chain, not only video overlays.
3. Compare current `out/.config`, `out/modules.order`, shipped module lists and
   boot packaging against the target. Identify every essential binary-only
   kernel module, missing source and kernel/userspace ABI requirement.
4. Map CPU topology, reserved memory, SCM/secure interfaces, RPMh, PMICs,
   regulators, clocks, pinctrl, interconnects, SMMU, UFS, USB, remote processors,
   thermal/BCL and charging. Separately map Adreno 725/KGSL, display/panel,
   touch, audio, WLAN/BT, video and camera.
5. Build a per-subsystem decision table: reuse target implementation, adapt
   Bouquet source, or investigate missing support. Include build dependencies,
   firmware expectations, userspace interfaces and concrete tests.

Exit gate: pinned source/dependency map, explicit blocker list and storage
budget. Missing a donor is not a blocker by itself; missing essential source or
an unresolved proprietary interface must be treated as a concrete engineering
problem rather than assumed compatible.

## Phase 2: Establish the 5.15 Core and Build

1. Prefer the verified Qualcomm Waipio 5.15 core over an unrelated SM8550 base.
   Fetch the selected core into this existing Git database after the space check.
2. Decide the integration mechanism from the actual graph and core delta.
   Retain the unified parent/history; do not force-push or reset the preserved
   branch. A targeted replacement of the work-branch core with recorded source
   provenance may be clearer than a giant conflict-heavy vendor merge.
3. Match the target compiler, common/vendor build relationship and module symbol
   requirements. Decide mixed GKI versus a downstream integrated build explicitly;
   vendor DLKM support alone does not require copying a donor's partition layout.
4. Add only the platform dependencies needed for minimal bring-up. Translate
   configuration deliberately rather than feeding the old defconfig through
   `olddefconfig` and accepting silently lost options.
5. Adapt build scripts for isolated 5.15 output. Preserve existing 5.10 workflows.
   Optional 5.10-specific KernelSU/SUSFS patches remain disabled until ported.

Exit gate: reproducible core, DT and required-module build; recorded generated
kernel release/config; resolved required symbols and coherent module versions.
Do not claim bootability at this gate.

## Phase 3: Minimal Safe Bring-Up

1. Validate GIC/timers, PSCI/CPU startup, console or another usable early logging
   path, reserved memory and persistent crash logging. Do not assume the
   reference GENI console is physically accessible on the phone.
2. Bring up RPMh, clocks, regulators, pinctrl, interconnects, SCM, SMEM, SMMU
   and UFS with conservative existing hardware limits.
3. Include essential thermal, BCL, watchdog and power protections before
   sustained on-device testing. No OC/UV, BCL disable or unverified PMIC writes.
4. Establish minimal initramfs/userspace and USB diagnostics before attempting
   a full Android boot. Record deferred probes, faults and firmware requests.

Exit gate: authorized minimal device boot with recoverable diagnostics, stable
storage enumeration and no unexplained SMMU faults or protection failures.

## Phase 4: Android and Peripheral Compatibility

1. Bring up remote processors, firmware loading, QRTR/RPMSG, modem interfaces,
   USB, CPU frequency/idle and suspend prerequisites.
2. Validate display/panel and touch, then KGSL/Adreno 725 with the installed
   userspace GPU stack. Preserve required ioctls and memory-sharing semantics.
3. Integrate WLAN/BT, audio, video and camera in dependency order. Use the
   matching Waipio/Ukee 5.15 code where verified; adapt Marble board data and
   source where necessary. Include charging, sensors, NFC and IR in coverage.
4. Test Android init/SELinux expectations, module load ordering, firmware paths,
   DMA buffers, secure buffers, codec/camera controls and HAL behavior.

Exit gate: repeated Android boots and subsystem tests, including suspend/resume,
charging/thermal behavior, calls/data, playback/recording and camera use.
Rebuild every kernel module for the target; do not reuse 5.10 `.ko` binaries.

## Phase 5: Restore Unified Features Incrementally

Use the frozen unified branch as the feature checklist. Reintroduce MGLRU,
compaction, Bluetooth tooling, NFC/Gunyah changes and other carried features
only after the corresponding 5.15 baseline subsystem works. Audit existing
target implementations before applying any backport to avoid duplicates.

Keep optional experimental behavior disabled during initial bring-up. Separate
feature restoration from core/platform fixes, with one focused change and a
targeted regression check per commit/package. Document intentionally deferred
features rather than claiming the original union is already preserved at runtime.

## Phase 6: Packaging and Release Validation

- Review boot/vendor_boot/DTBO/vendor_dlkm changes independently before editing.
- Inspect existing device headers, partition limits, DT selection, ramdisk/module
  ordering and AVB expectations. Do not copy P11 header v3 or super-image values
  merely because the reference config sets them.
- Record actual build commands after scripts are adapted. The current baseline
  commands are `./build_bouquet.sh --noccache` and `./package_bouquet.sh`; running
  them unchanged is not proof of a correct 5.15 build.
- Verify generated kernel release, module vermagic/symbols, artifact sizes,
  DT contents, SHA-256 checksums and ZIP integrity with `7z t`.
- Keep new outputs under this checkout's `release/` and new packages under
  `consolidation/packages/`, with explicit 5.15 experimental names.
- Confirm the existing recovery backup and known-good artifact paths before
  any separately authorized flash. Document the recovery route first.
- Run repeated cold boots, suspend/resume, storage and peripheral regression
  tests before calling the port usable. Update the selected 5.15 base's security
  fixes before general use; 5.15.149 is a starting reference, not a current
  security-status claim.

## Immediate Next Work

Start Phase 1 with remote manifest/revision discovery and full platform-DT,
KGSL and essential-module source mapping. Do not download another whole BSP,
change kernel code, build or flash as part of merely establishing this plan.
