# Marble Qualcomm 5.15 Port Plan

Date: 2026-09-07

## Current Status

The working source on `port/waipio-5.15-20260907` has now transitioned in place
to the pinned Qualcomm 5.15.149 core, with the retained Marble board sources and
reviewed initial Cape adapters. Source changes and validated probe work are now
recorded in port-branch checkpoints; the GitHub-backed unified source remains
unchanged. The older sections below record the
research chronology, not a claim that the checkout still contains the 5.10 core.

The latest full ThinLTO build passed at source commit `df49dd0418b9`: native vmlinux,
core/module BTF, modpost, 146 configured modules and the arm64 Image. CFI and SCS
remain enabled. Artifact hashes were independently verified against the build
manifest. This validates the current build configuration, not complete Marble
hardware support or bootability. Packaging and device gates remain closed.

FullLTO was an unnecessary donor-default choice and caused resource escalation;
ThinLTO matches the proven 5.10 baseline. Diagnostic DT composition passed and
its 139 warnings match frozen baseline under the same compiler.

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

Continue Phase 1 from the findings below. The core integration gate remains
closed until the boot-critical adaptations, build strategy and storage budget
are defined. Do not download another whole BSP or overwrite the current core
merely because candidate branch revisions have been resolved.

## Phase 1 Progress: 2026-09-07

Candidate pins are recorded in `port-5.15-sources.json` at repository root.
These are 14 independently resolved branch heads, not a proven coherent BSP
release. The initial pass read remote refs and selected source files only.
The subsequent bounded core acquisition is recorded below. No kernel code changed.

### Manifest and Build Findings

- Public `kernel/manifest` and `kernel/superproject` head queries returned no
  P11-named branches. This does not rule out release tags or another manifest
  repository; discovery is incomplete rather than a claim no manifest exists.
- The related EOS manifest at `958c54560112ac3c67c12c5af30e0c94b9c565b2`
  identifies the real external KGSL repository as `kernel/msm-modules/graphics`
  and audio as `kernel/msm-extra`. Its device links select Monaco and EOS;
  it must not be used as a Marble configuration or P11 release lock.
- Matching P11 kernel/common and kernel/build branches do exist. Both core
  Makefiles report 5.15.149. Matching version strings do not establish KMI.
- Both core `build.config.constants` files specify `CLANG_VERSION=r450784e`.
  The pinned compiler repository contains that version. The Qualcomm config
  uses `prebuilts/clang/host/linux-x86` and linux-x86 build tools.
- This host is `aarch64`; installed compiler is Ubuntu Clang 18.1.3 targeting
  aarch64. Do not download x86 prebuilts and assume they run natively, or assume
  Clang 18 is equivalent to the reference compiler for CFI/LTO/module builds.
- Free space at inspection was approximately 29 GiB (92% filesystem usage).
  No tracked background jobs were listed. A complete fetch/build storage budget
  has not been established, so no core Git objects were fetched.

### Concrete Platform Mapping

Target paths below use the immutable kernel and module revisions in the JSON.
Local paths refer to the preserved unified source, not a future 5.15 result.

| Area | Local source requirement | Target evidence | Decision / next check |
| --- | --- | --- | --- |
| Full platform DT | `ukee.dtsi` includes `cape.dtsi`; Marble overlays add board data | Pinned platform-DT `qcom/` listing has Waipio files but no Cape/Ukee-named files; `waipiop.dtsi` selects ID 482, not 591 | Adapt the actual Cape/Ukee/Marble chain; do not substitute Waipiop based on its name |
| Pinctrl | `cape.dtsi:603-612` selects `qcom,cape-pinctrl`; `drivers/pinctrl/qcom/pinctrl-cape.c:16-27` uses Cape tables | Target `qcom-msm-pinctrl.c:50-53` matches only Waipio and Waipio-VM; its Makefile has no Cape entry | Compare table/framework changes, then port Cape binding/data; no blind compatible rename |
| UFS PHY | `xiaomi-sm8475-common.dtsi:65-77` selects Cape; local `phy-qcom-ufs-qmp-v4-cape.c:304` implements its match | Target PHY Makefile line 10 builds Waipio and other variants, not Cape; inspected Waipio driver matches Waipio | Compare Cape calibration, sequencing and framework APIs before adapting source; no speculative PHY values |
| GPU | `cape-gpu.dtsi:29-31` and local `adreno-gpulist.h:331-360` use `qcom,adreno-gpu-gen7-4-0`; Ukee reports Adreno725v1 | External graphics `adreno-gpulist.h:2256-2272,2604` contains and registers the exact Gen7-4-0 core, with BCL and GMU operations | Positive source match; firmware versions, UAPI and memory-sharing behavior still need comparison |
| Thermal/BCL | Current config and build lists include TSENS, PMIC5 BCL, SOC BCL and cooling | Target `drivers/thermal/qcom/Makefile:2-15` contains corresponding build entries | Preserve protection coverage; audit Kconfig and DT bindings before device use |
| Video | Ukee-specific platform required | Matching video DT and explicit Ukee handling previously verified | Useful leaf support, not a replacement for full platform DT |

### Early Boot and Artifact Boundary

- Existing `out/.config` and `out/modules.order` remain 5.10 artifacts. They are
  evidence of the previous build shape, not proof that current sources or any
  candidate target were compiled.
- `build_bouquet.sh:427-516` stages foundational and safety modules into vendor
  boot: clocks, interconnects, SMMU, Cape pinctrl/PHY, UFS/crypto, RPMh, SPMI,
  TSENS and PMIC5 BCL. Exact runtime dependency closure remains to be computed.
- UFS and its transitive module dependencies cannot live exclusively on the
  filesystem they are required to mount. GPU inclusion in the old vendor boot
  payload does not prove GPU is required for storage bring-up.
- KGSL and several remoteproc/thermal/display modules are copied to both
  payloads; audio, WLAN, camera, video and data modules have vendor_dlkm-only
  assignments in `build_bouquet.sh:532-587`. Assignments are not load-order proof.
- No essential binary-only driver has been demonstrated in this bounded source
  inspection. A full module-to-source inventory is still outstanding.
- Packaging compares release files against `out/`, not the source revision
  (`package_bouquet.sh:47-97`). Mutually matching stale artifacts are possible.
  Also, `strip_kmod` callers in `build_bouquet.sh:621-635` do not explicitly
  abort on every missing listed module. Before a 5.15 package, add fail-closed
  checks for the required module set and build provenance after focused review.

### Next Bounded Work

1. Compare Cape versus target pinctrl and UFS PHY data/API requirements using
   only selected files. Obtain focused review before any UFS or platform edits.
2. Resolve native ARM64 toolchain/build execution and mixed-GKI versus integrated
   build requirements. Audit actual symbol exports and vendor-hook dependencies.
3. Complete the boot module-to-source/dependency table and platform-DT include
   closure. Continue manifest discovery without syncing the whole watch BSP.
4. Define the remaining external-source and build-output budget. The core-only
   acquisition below is complete; it does not authorize a full BSP sync or build.

## Cape Comparison and Core Acquisition

The next bounded pass compared frozen Bouquet files directly against the pinned
5.15 core. A focused read-only reviewer checked the UFS and pinctrl contracts
before any proposed hardware-facing edit. No such edit has been applied.

### UFS Adaptation Decision

- The complete Cape `.c` file is identical to target Waipio `.c` after replacing
  the Cape identity strings with Waipio. This was checked programmatically,
  not inferred from similar function names. Source SHA-256 values are in the JSON.
- The headers are NOT interchangeable. Cape uses PHY base `0x400`, PCS2 base
  `0x800`, TX base `0x1000 + 0x800*n` and RX base `0x1200 + 0x800*n`.
  Target Waipio uses `0xC00`, `0x200`, `0x400 + 0x400*n` and
  `0x600 + 0x400*n`, respectively. RX clock-edge control is bit 6 versus bit 5.
- Candidate first PHY integration: retain the Cape `.c/.h` data and binding,
  add the target Kbuild entry, and use the matching target common PHY core/header.
  Do not substitute Waipio calibration values or invent a new rate.
- Target common PHY fields and register-save callback are additive; the callback
  is null-checked. New optional supply handling must still agree with Marble DT.
- Target UFS defaults to G4 submode (`ufs-qcom.h:71`). It can select G5 for
  controller major 5 plus device version 4.0 (`ufs-qcom.c:4717-4727`), or via DT.
  Cape's `!!submode` would incorrectly interpret G5 as G4. Before runtime use,
  prove or enforce the Cape-only NON_G4/G4 contract; do not enable G5.

### Pinctrl Adaptation Decision

- Comparison of all 210 explicit GPIO `PINGROUP` entries found one difference:
  GPIO98 has the additional `pll_clk` function on Cape. Its function group list
  includes GPIO98 and GPIO107; Waipio includes only GPIO107.
- PDC wake mappings and QUP I3C register mappings match after whitespace removal.
  Function-list reordering is not itself a change of mux IDs because `FUNCTION`
  uses designated enum indices (`pinctrl-cape.h:6-11`). Preserve Cape tables.
- The target framework retains the probe/remove signatures and existing table
  fields. It adds direct-connect and spare-register support; absent spare-register
  data is guarded, but direct-connect data is not safe with extra platform IRQs.
- `cape.dtsi:603-612` defines one TLMM IRQ. The target core dereferences
  `soc_data->dir_conn[i]` for extra IRQs (`pinctrl-msm.c:2064-2074`); verify the
  final composed target DT still has only the base IRQ before using null data.
- Candidate first pinctrl integration: Cape tables and a normal-host wrapper.
  The old wrapper's `trace/hooks/gpiolib.h` dependency is absent from the target.
  Do not claim Cape-VM support by retaining its compatible while removing the
  VM GPIO-read hook or ignoring its irqchip dependency.

### Board Include Closure

The read-only `tools/port_515_dt_inventory.py` tool inventories `ukee.dts` and
`marble-sm7475-pm8008-overlay.dts` from the frozen Git revision. It resolves
literal includes and tracked symlink targets without checking out source or
writing files. Output includes every path, Git blob ID, byte count and include
edge; unresolved roots/includes and invalid refs fail with nonzero status.

The corrected closure has 69 entries (67 regular files and two symlinks), totaling
661,631 Git blob bytes, with no unresolved includes. The original 67-entry scan
counted symlink bodies but failed to follow these two targets:

- `include/dt-bindings/input/linux-event-codes.h` ->
  `include/uapi/linux/input-event-codes.h`.
- `include/dt-bindings/arm/msm/qcom_dma_heap_dt_constants.h` ->
  `include/linux/qcom_dma_heap_dt_constants.h`.

This remains an import inventory, not proof that those files compile against
5.15 bindings. Conditional includes are not evaluated. The initial DT scan found
no literal UFS gear/submode-limit properties, so host defaults cannot be ignored.

Validation: the tool succeeds on frozen Bouquet with the counts above, reports
both missing Marble DT roots on the unported 5.15 core (exit 1), and rejects an
invalid revision without writing files (exit 2). Run it with
`python3 -B tools/port_515_dt_inventory.py`; output is JSON on standard output.

### Core Acquisition Result

- Acquired only commit `603278d412c33acb62666bf8e09ccfc0b8732165` into
  `refs/port/waipio-5.15-base`, with depth one, no tags and no checkout.
- Used a tracked serial background fetch with reduced CPU/I/O priority, two
  pack threads, automatic maintenance disabled and a 4 GiB per-file limit.
  That limit is not a total repository quota. Exact command is in the JSON.
- Reported packed Git storage increased from 2.77 GiB to 2.98 GiB, about
  0.21 GiB. Free space still reported 29 GiB. No duplicate source checkout,
  archive, local backup or compiler download was created.
- `.git/shallow` contains only the new target commit. The target's ancestors
  are intentionally unavailable; do not base merge-base/history conclusions on
  this shallow snapshot. Existing unified and port branch hashes are unchanged.
- The working tree remains 5.10 plus research changes. The acquired 5.15 tree
  can now be inspected through Git without repeatedly downloading source files.

### Initial Build Strategy and Size

Use one Qualcomm downstream 5.15 core and rebuild all required modules with the
same native compiler for the first compile/bring-up milestone. Do not acquire a
second Common checkout or build output yet. Target Makefile lines 702, 1288 and
1544-1545 retain normal vmlinux/module builds when `KBUILD_MIXED_TREE` is unset;
the target GKI defconfig enables module versioning. This is a selected development
strategy, not a completed configuration or a GKI certification claim.

Tree metadata reports 71,028 target files totaling 1,111,825,061 blob bytes.
Against the frozen unified source, 38,791 paths have identical blobs, 26,723
shared paths differ, 16,956 paths are baseline-only and 5,514 are target-only.
These counts do not authorize dropping baseline-only drivers or importing every
shared-file change blindly. They provide a measured source-size bound and show
why a controlled core transition with explicit retained-source inventory is
preferable to an unreviewed overwrite. Build-output space remains unbudgeted.

## Native Compiler and Baseline Dependency Results

### Compiler Candidate

Native Ubuntu Clang/LLVM/LLD 18.1.3 and the versioned `-18` tools are installed.
No other Clang version was found in the bounded command/package inventory.
GCC 13 is available but is not a substitute for this legacy Clang CFI/LTO setup.

The target Makefile at lines 451-480 does NOT implement `LLVM=-18` suffix
selection: any nonempty LLVM value selects unversioned tool names. Use `LLVM=1`
with explicit `CC=clang-18`, `HOSTCC=clang-18`, `HOSTCXX=clang++-18`,
`LD=ld.lld-18`, and versioned AR/NM/OBJCOPY/OBJDUMP/READELF/STRIP assignments.
The complete proposed assignment list is recorded in the source JSON.

A native ARM64 compiler smoke test compiled a small indirect-call function to
LLVM IR using ThinLTO, split LTO units and the target legacy cross-DSO CFI flags.
The output contained `llvm.type.test`, `__cfi_slowpath_diag`, `Cross-DSO CFI` and
`EnableSplitLTOUnit`, without compiler diagnostics. This is stronger than driver
flag acceptance but does not prove a kernel link, module ABI or runtime behavior.
No replacement of legacy CFI by KCFI, or disabling of CFI, is part of this plan.
An initial bitcode-disassembly test wrote two small diagnostic files because of
split-LTO output behavior; only those test-generated files were removed, and
the successful rerun emitted textual IR directly to memory with no output files.

### Existing Artifact Dependency Graph

The current checkout's `release/` is absent, and `out/` has no `.ko`,
`modules.dep` or `modules.softdep` artifacts in the bounded search. That is NOT
absence of all baseline artifacts: the existing preserved
`../Bouquet_marble_release/vendor_boot_modules/` contains the actual modules.

Read-only `modinfo` traversal of seven seeds (UFS host, Cape PHY, Cape pinctrl,
ARM SMMU, PMIC5 BCL, SOC BCL and TSENS) resolved a 25-module hard dependency
closure without unresolved hard dependencies. All observed vermagic strings
report `5.10.258-Bouquet-v4.9 SMP preempt mod_unload modversions aarch64`.
The full normalized graph and observed softdeps are recorded in the JSON.

Important edges include:

- UFS host -> SCM, QTI crypto, common PHY, Qualcomm clocks, IPC logging and
  UFS QTI crypto; QTI crypto also reaches HWKM and TMECOM.
- Cape PHY -> common PHY; Cape pinctrl -> common pinctrl -> SCM.
- ARM SMMU -> SCM, IOMMU utilities, IOMMU logger and secure buffer.
- PMIC5 BCL and TSENS -> IPC logging -> minidump -> SMEM.
- Common clocks -> GDSC -> debug regulator and proxy consumer.

This is not a complete boot list. For example, the Cape PHY is selected through
a DT/provider relationship rather than a hard dependency naming it from UFS host.
Regulators, clocks, SMMU suppliers and PMIC parents require a DT/probe graph too.
Built-ins need no module depends entry. The observed PDC, HW-spinlock and VM
irqchip softdeps were recorded separately rather than silently treated as hard
dependencies or removed. The 5.15 modules must generate their own graph.

### Next Implementation Boundary

Core acquisition and candidate native-toolchain selection are complete. Next:
map the measured baseline module closure onto target source/Kconfig exports,
prepare the exact retained-board/driver inventory, and budget a small compile
output. Then transition this same work branch to the selected core with an
explicit source delta. Start with compile preparation and the reviewed Cape
adapters; do not run the old packaging workflow against a mixed artifact set.

## In-Place Transition and First Compile Milestone

### Source Transition

`tools/port_515_transition.py --apply` checked the source delta before applying
it directly to this checkout. It retained the 35 board DT sources absent from
the target plus the four Cape pinctrl/PHY files. It preserved the workflow
scripts, ignore rules and consolidation records. No saved patch, archive or
duplicate checkout was made. The Git index and branch tips were not changed.
The helper now refuses a repeat transition because the worktree has changed.

This is a large uncommitted core transition, not a merge commit. Baseline-only
features not present in the initial core remain recoverable from the frozen
unified Git history and its GitHub branch. They are not claimed restored on 5.15.
Target-only source paths are currently untracked: a later commit must account
for those paths and any matching ignore rules, not blindly omit them.

### Applied Adapters

- Added a physical-Cape pinctrl Kconfig/object entry. Removed the unavailable
  VM GPIO hook and VM binding rather than pretending that VM behavior works.
  Negative IRQ-count errors propagate; anything other than one IRQ fails probe.
- Added the Cape PHY to the V4 build list. Its calibration path now rejects
  unknown/G5 submodes before asserting reset or applying calibration tables.
- Kept both Cape pin tables and PHY register/calibration headers byte-identical
  to the frozen baseline. Git blob comparisons passed after the transition.
- Restored GPUCC frequency-limiter reset ID 8 together with its matching
  register `0x9538`, bit 0 mapping. Target generic reset readback behavior still
  differs from baseline; IRQ-clear timing remains a hardware validation item.
- Kept target shared headers, including renumbered RPMh and thermal definitions.
  Copied symbolic DT references compile against those target definitions.
- Added `arch/arm64/configs/vendor/marble_515_bringup.config` for missing module
  gates and their explicit infrastructure dependencies. It is a compile fragment,
  not a complete runtime configuration.
- Guarded both legacy 5.10 workflow scripts before output writes. Their required
  commands were tested on the new source and refused as intended:
  `./build_bouquet.sh --noccache` and `./package_bouquet.sh` both exit 1.

### Build and DT Results

Run the limited workflow with:

```bash
nice -n 10 ionice -c 2 -n 7 bash tools/port_515_prepare.sh
```

It merges target GKI, Waipio and the new Marble compile fragment, runs
`olddefconfig`, checks every mapped module gate and CFI/SCS requirements, runs
`modules_prepare`, and compiles three selected objects. It then preprocesses and
compiles the retained base and overlay using target headers/DTC, composes them,
and verifies the physical TLMM binding/IRQ contract. It does not build an Image,
link modules, create a package or access the device.

| Check | Result |
| --- | --- |
| Source Makefile | 5.15.149 |
| Generated kernel release | `5.15.149-marble-5.15-probe+` |
| Compiler | Native Ubuntu Clang 18.1.3 |
| Required module gates | All 25 mapped modules have controlling symbols enabled as m/y |
| Security/build settings | Legacy CFI and SCS enabled, CFI permissive disabled, target GKI FullLTO retained |
| Selected objects | Cape pinctrl, Cape PHY and Waipio GPUCC compiled successfully as LLVM IR bitcode |
| Diagnostic DTs | Base and overlay compiled and composed successfully |
| SoC ID | `0x24f` = 591, Ukee |
| Physical TLMM | `qcom,cape-pinctrl`, interrupt cells `0 d0 4`, exactly the reviewed base IRQ |
| DTC warnings | 139 warning lines, not yet triaged against a baseline compiler run |
| Probe output size | 25,404 KiB, approximately 25 MiB, including diagnostic DTs |
| Free space after probe | Approximately 29 GiB |
| Old generated release | Still `5.10.258-Bouquet-v4.9` under untouched `out/` |

Logs are under `consolidation/scratch/port-515-prepare-first.log` and
`consolidation/scratch/port-515-prepare-dt.log`. Output is isolated under
`out-5.15-probe/`, now ignored by Git. Initial execution requires 20 GiB free,
uses two workers, sets a 1 GiB per-file limit, and checks a 2 GiB total output
budget after the limited probe. This is not a full-build disk allocation.

### Remaining Gates

The objects are LTO bitcode, not linked `.ko` files. No full vmlinux/modpost,
module dependency generation, BTF generation or boot-image build has passed yet.
Pahole is not currently in PATH. Diagnostic DT compilation emitted nonfatal
warnings, including reg/ranges and unit-address checks; do not call it schema
clean or flash-ready. The hardware and proprietary userspace gates remain open
work, not reasons to assume the port is impossible.

The following milestone completes that bounded compilation expansion and
controlled warning comparison. Full packaging and device writes remain blocked
until their separate validation gates are satisfied.

## Boot-Module Prelinks and Controlled DT Comparison

### Expanded Compilation

The source inventory now records exact Kbuild object names for all 25 mapped
modules; underscore/hyphen names are not guessed. The prepare workflow builds
those objects plus GPUCC, then invokes each native `.lto.o` prelink separately.
It requires all those module gates to resolve to `m`, retaining FullLTO, legacy
CFI and SCS with permissive CFI disabled. Compilation uses two workers; prelinks
run serially with one make job and target-specific `--threads=1` passed to LLD.
The generated ARM SMMU link command confirms the thread limit was honored.

The first expanded compile found an implicit-int declaration in target
`drivers/soc/qcom/minidump_log.c:119`. Changed `static md_align_offset;` to
`static int md_align_offset;`, preserving its implicit-int semantics and the
type already used by the frozen 5.10 baseline. No warning suppression, minidump
disable or security configuration reduction was used. The subsequent run passed.

All 26 module-sized prelinks were verified as ELF64 little-endian AArch64
relocatable objects (`ET_REL`), exercising native LTO code generation rather
than merely stopping at bitcode/thin archives. All 26 also define `__cfi_check`;
this is not a runtime CFI test. Native outputs total 20,049,008
bytes. The complete probe directory, including comparison data, uses 64,512 KiB
(about 63 MiB), with approximately 29 GiB free on the filesystem.

`out-5.15-probe/boot-link-report.json` records configuration SHA-256, each native
object hash/size and its undefined-symbol names. There are 696 distinct undefined
names across the prelinks. Relocatable links intentionally leave core/provider
imports unresolved: this is neither a missing-module count nor proof that all
imports are valid. No global symbol/CRC/namespace check, modpost, final `.ko`,
full `vmlinux`, Image or BTF generation was performed.

Logs: `consolidation/scratch/port-515-bootlinks-first.log` records the compiler
failure; `consolidation/scratch/port-515-bootlinks-retry.log` records the successful
compile/prelink run and diagnostic DT checks.

### Controlled Warning Comparison

`tools/port_515_dt_compare.py` preprocesses and compiles both frozen and current
DT inputs with the same native Clang 18, target-built DTC and flags. Frozen Git
blobs, including symlink targets, are supplied through anonymous RAM-backed
files and an LLVM virtual filesystem with fallthrough disabled. No frozen source
checkout, archive or persistent source copy is created. Unprocessed DTC includes
or incbin directives are rejected to avoid accidentally reading current files
while claiming an isolated baseline. Quoted preprocessor paths are not mistaken
for include directives.

The comparison uses category/message multisets, excluding file/line prefixes:

| Result | Count |
| --- | --- |
| Frozen baseline warnings | 139 |
| Current port warnings | 139 |
| Added warning messages | 0 |
| Removed warning messages | 0 |

The current comparison DTB hashes match the existing probe artifacts. The frozen
Ukee DTB differs from current output, while the Marble board overlay matches;
the baseline was not accidentally compiled against the current shared headers.
The result establishes that these diagnostics reproduce on the frozen source
under the same toolchain, not that inherited reg/ranges or other warnings are
harmless, schema-compliant or acceptable for every 5.15 driver.

Report: `out-5.15-probe/dt-warning-comparison.json`.
Log: `consolidation/scratch/port-515-dt-warning-comparison-retry.log`.

### Next Full-Link Gate

Budget the full core and enabled-module build separately from the 2 GiB probe
allowance. Provide a suitable native `pahole` for the selected BTF configuration
rather than silently dropping BTF. Then build the full core and modules to obtain
fresh 5.15 symbol/CRC data, run modpost and final linking, and evaluate remaining
compiler/linker failures. Do not reuse the old 5.10 `Module.symvers` or modules.
Full DT supplier/peripheral coverage and runtime/firmware interfaces still need
validation. No device writes are authorized.

## Commit Policy

From the 2026-09-07 source checkpoint onward, commit each validated batch,
including the accumulated source transition and probe tooling. Generated build
outputs, logs, downloaded/extracted toolchains and leftover compiled utilities
are not source changes and must remain outside commits.

Initial staging is audited against the pinned 5.15 tree, not performed with a
blind add-all. In particular, `fs/ext4/.kunitconfig`, `fs/fat/.kunitconfig` and
`lib/kunit/.kunitconfig` are upstream source files hidden by existing ignore rules
and must be explicitly staged. The three leftover Bluetooth/NFC executables are
preserved locally and ignored, not committed or deleted. The original unified
branch remains the independent 5.10 reference.

## Full Build Preparation

The initial source checkpoint is `c8a630a72c65e5ea49dc979ae201a75357a2e664`.
The next batch adds project-local BTF tooling and a guarded full-build runner.

Native Ubuntu `pahole` 1.25 was downloaded and extracted under
`consolidation/toolchains/pahole-1.25-arm64/`, not installed system-wide.
Package and executable SHA-256 pins are recorded in the source inventory; the
existing native runtime libraries satisfy its dependencies. The preflight
successfully compiled a small native ELF and encoded a nonempty BTF section.

Run the full workflow with `python3 -B tools/port_515_build.py` through a tracked
background process. It requires committed, clean source and reuses
`out-5.15-probe/`; no second build cache is created. It runs `olddefconfig`,
`vmlinux`, `modules`, then `Image`, checking required config gates, core BTF,
symbol tables, module order/vermagic and the arm64 Image header. Its report
records source/config/tool hashes, completed phases, failures and resource data.

Per the user's instruction, compilation uses all four cores (`-j4`). Core
linking and BTF encoding use four workers. Module builds run four jobs with one
worker per linker/BTF encoder, avoiding sixteen-worker oversubscription.
`tools/port_515_ld.sh` preserves every architecture linker argument while adding
the phase's worker limit. Existing version-specific pahole flags are preserved.

The first full attempt completed C compilation but failed in FullLTO code
generation with `LLVM ERROR: out of memory` under the initial 18 GiB address-space
cap. Its source was commit `a37b8cfa273815d4a5cb8e07de5f1248c74a73ee`; the log is
`consolidation/scratch/port-515-full-build-a37b8cfa.log`. Compiled objects remain
available, so the retry need not repeat the full source compilation.

Per the user's updated resource instruction, use available host RAM/swap and
disk rather than the initial arbitrary caps. The runner now keeps only a 2 GiB
free-space stop, removes added memory/per-file/fixed-output caps, and disables
core dumps. Four FullLTO code-generation partitions are requested for core
linking; the module phase retains four parallel jobs with one partition each.
CFI, SCS and FullLTO stay enabled. Free-space checks are output-driven and at
phase boundaries, not a filesystem quota. No baseline artifact cleanup is used.

Final experimental outputs are explicitly refreshed before their build phases
and checked for new mtimes, preventing stale final artifacts from passing as a
new build. Compiled objects are retained. The small BTF compiler probe is also
process-group managed, and both full/probe workflows share an output lock.

Preflight log: `consolidation/scratch/port-515-full-preflight.log`.
Full-build manifest: `out-5.15-probe/full-build-report.json`.
Packaging and device writes remain separate, closed gates.

## Resized Host Resume

On 2026-09-08 the user increased the host to 16 available CPUs and approximately
62 GiB usable RAM (64 GiB nominal). Inspection found approximately 33 GiB free
disk, a clean port branch at `428f29192ac5`, preserved build cache and no surviving
compiler/linker processes. The prior run has no validated completion record and
is treated as interrupted, not successful.

Full and probe runners now discover available CPU capacity rather than hard-code
four jobs. On this host, compilation uses 16 jobs, core linking uses 16 threads
and FullLTO partitions, and core BTF uses 16 workers. Module builds use 16 jobs
with one linker/BTF worker each. The 2 GiB free-space stop remains; no artificial
memory or per-file caps are added. Existing compiled objects are reused.

## Successful ThinLTO Full Build

The ThinLTO correction was committed as `a607ebc9dff7`. Its first full run linked
vmlinux with BTF, then exposed an inconsistent LLCC programming routine during
module compilation. That attempt took 540.6 seconds in total; it was not a full
success. No security feature was disabled to get past it.

Commit `3b210d25e13c` restores the LLCC programming function byte-for-byte from
the frozen working baseline, restores its v31 shift constant, removes an unused
orphan helper, and adds the Cape compatible using the existing Waipio config.
All 23 target Waipio rows were verified equal to the frozen Cape values, so no
cache allocation values were changed. The object compile and full build passed.

The successful cached retry completed in 182.7 seconds with all 16 CPUs available.
It produced vmlinux, core/module BTF, fresh symbol tables, 135 final modules and
the arm64 Image. Peak recorded child-process RSS was 8,161,872 KiB (about 7.8 GiB);
this is not an aggregate process-memory measurement or a cold-build time claim.

- Source commit: `3b210d25e13cc8d5f6d30e0fc02e4ee03b031a19`.
- Release: `5.15.149-marble-5.15-probe+`.
- Image: `out-5.15-probe/arch/arm64/boot/Image`, 42,433,024 bytes.
- Report: `out-5.15-probe/full-build-report.json`.
- Log: `consolidation/scratch/port-515-full-build-thin-3b210d25.log`.
- Independent hash checks passed for Image, vmlinux, config, Module.symvers and
  all 135 final modules. Module architecture, BTF and vermagic checks passed.
- Output/cache usage was approximately 5.43 GiB; about 27.9 GiB remained free.

This is the first successful full-build milestone, not a flashable release.
Device-specific external graphics, multimedia and connectivity integration,
complete DT supplier/module-loading coverage, boot packaging and runtime tests
remain separate work. No ZIP was created and no device was flashed.

## Boot Provider Integration

Read-only inspection of the recovery images established boot/vendor_boot v4 and
4096-byte alignment. The vendor ramdisk contains a PLATFORM fragment of
11,388,309 compressed bytes and a named DLKM fragment of only 104 bytes. The
actual 298 module files and load metadata reside in the PLATFORM fragment.
Its normal load list has 102 entries / 100 unique names; 75 matched the initial
5.15 module set by normalized basename. Name matching is not proof of equivalent
providers. Recovery loads a much wider set than normal boot.

The preserved release manifest reports Clang/LLD 22.1.7, whereas the retained
`out/.config` records Clang 18.1.3. These are distinct historical artifacts; do
not claim they establish identical build provenance solely from kernel.release.

The known-good ZIP installer was read directly in memory. It writes boot,
vendor_boot, vendor_dlkm and DTBO, patches vbmeta, and has legacy KSU/GPU handling.
Those behaviors are not approved unchanged for 5.15. The absent unpacked template
does not prevent read-only inspection of its preserved ZIP, but no installer or
partition-writing logic is changed in this batch.

The current batch enables existing downstream GENI UART/I2C/SPI, MSM GPI DMA,
I2C PMIC, PM8008 regulators, PMIC GPIO/MPP, ADC5/ADC7 and temperature-alarm modules.
Exact retained DT strings match these provider implementations. No regulator
voltages, cache tables or thermal policies are changed. Configuration preflight
passed; the full runner now requires the new module gates and output paths.

GENI remains an explicit runtime integration gap: the retained wrapper is flat,
uses `qcom,qupv3-geni-se` and `qcom,wrapper-core`, while target clients expect the
`qcom,geni-se-qup` parent hierarchy and parent clocks/interconnects. The matching
5.15 vendor DT source migrates only part of that layout, so it is not safe to
copy wholesale. Console support is also not inferred from SERIAL_MSM_GENI=m;
its old console Kconfig symbol is absent. Provider compilation is a step toward
integration, not a declaration that these buses probe successfully.

`tools/port_515_boot_inventory.py` reads the v4 fragments/CPIO metadata in memory
and compares them with a successful 5.15 build's module dependency graph. It
writes a small audit report only, not an image, module payload or flash package.

### Provider Milestone Result

The full build at `3252352eda8f` passed in 176.9 seconds and produced 146 modules,
up from 135. All ten explicitly required provider module paths, their selected
dependencies, core/module BTF, vermagic and final Image checks passed. Independent
hash verification also passed for every module and the core artifacts.

The boot inventory tool ran successfully against both preserved vendor fragments.
It confirmed 298 modules in PLATFORM and none in the 104-byte DLKM fragment.
Of 100 unique normal-load names, 76 now match by normalized basename. Starting
from those matches and the newly selected providers yields an 87-module hard
dependency closure with no unresolved hard imports. The 24 unmatched load names,
soft dependencies, DT supplier relationships and runtime bindings are explicitly
outside that closure claim. It is not a final early-boot load list.

The built MSM UART module exposes a tty alias but no OF modalias; neither UART
autoload nor console readiness is claimed. The next concrete source task is the
GENI wrapper/engine layout and clock/interconnect contract, followed by a revised
early-boot load plan. A flash ZIP would be premature before those are resolved.

Build log: `consolidation/scratch/port-515-build-providers-3252352e.log`.
Audit report: `out-5.15-probe/boot-integration-report.json`.

## GENI Interface Adaptation

The retained flat GENI layout is migrated to the target parent/child model.
All 46 engines, including disabled alternatives, now belong to their original
wrapper. The three wrappers match `qcom,geni-se-qup`, have original AHB clock
IDs and identity address translation, and retain their IOMMU stream IDs, DMA
coherency/address-pool properties and MMIO resources. The three root GPI nodes
are unchanged. Engine addresses, IRQs, pinctrl, clocks, DMA channels, rates and
status are preserved by source and compiled-DT checks.

ICC votes now live on each engine. Core and DDR endpoints are retained, including
QUP1's aggre1 route. The configuration vote is the real existing APPSS-to-QUP
route, not a rename of the old memory-facing SNOC-to-LLCC path. The console uses
only the two ICC slots its upstream driver requests.

The debug UART uses the upstream driver and `ttyMSM0`; downstream MSM GENI keeps
HS UART and vendor ioctls at `ttyHS0`. New opt-in console-only mode prevents the
upstream driver from registering a second ttyHS driver. Normal mode is unchanged;
registration and error unwind were tested with isolated stubs across both modes,
console states and registration failures. The downstream OF table is exported
for module alias generation. `serial0`, `hsuart0` and I2C aliases are preserved,
and stdout uses `serial0:115200n8` rather than the obsolete absolute node path.

`tools/port_515_geni.py --build` compiles/composes diagnostic DTs and validates
them against the small frozen hardware-metadata fixture. Source/DT/config checks
passed before the build checkpoint. Hardware transactions, suspend/resume and
userspace platform sysfs paths still need device validation; reparenting changes
those platform paths even though bus aliases and device identities are retained.

### GENI Validation Result

The full build at `df49dd0418b9` passed in 178.5 seconds: vmlinux, core/module BTF,
146 modules and Image. All artifact hashes were independently checked. The new
HS UART OF aliases are present. The source/compiled-DT invariants and aliases
also passed as a required build phase, and injected register, IOMMU and wrapper
clock corruption was rejected by the checker.

The old normal-load name `msm_geni_se` is now accounted for by the built-in
`qcom_geni_se` provider after successful GENI DT adaptation. It does not require
a replacement `.ko` load entry. This is an explicit integration mapping, not a
general basename-renaming rule. Other unmatched boot entries remain separate.

The identified GENI source-level blockers are resolved. On-device UART/I2C/SPI
transactions, Bluetooth UART ioctls, suspend/resume and userspace sysfs paths
still require validation; no hardware operation or flash is claimed.

## Boot Stabilization Batch

Active DT clock nodes request `qcom,cape-gcc` and `qcom,cape-dispcc`, while the
initial 5.15 drivers matched only Waipio. Restore Cape-specific PLL/data/probe
fixups from preserved baseline `1efb1f3077c7dd17bb24a9397080eb3464b8f7ff` into
the existing drivers and GCC binding header. Use `of_device_is_compatible()`;
leave the unrelated video AXI branch-op differences out. A raw source comparison
against the preserved files confirms those are the only intentional differences.

The preserved device DT, not merely adjacent source, requests UFS maxima of
850 MHz. For this unbooted 5.15 stabilization profile, cap core/ICE/UniPro maxima
and all six turbo frequency properties to the existing 300 MHz table point.
This is an explicit temporary bring-up policy. Full Cape clock data is retained
for correct hardware interpretation; no new rate or voltage value is invented.

Enable the retained CPU pause/hotplug cooling drivers, the USB nop-PHY referenced
by active DWC3, and the PMK8350 RTC driver. Thermal trip tables and policies are
not changed. Other old module names are classified explicitly as alternate-SoC,
optional or deferred peripheral/OEM interfaces rather than copied blindly.

`tools/port_515_load_plan.py` generates a diagnostic normal-boot candidate from
the matched baseline entries, selected providers and USB diagnostics. It closes
both hard and pre/post soft dependencies, rejects missing providers/cycles,
checks Cape clock module aliases and UFS caps, and verifies planned module hashes.
It emits only a load list and JSON inventory. It does not copy modules, create
images, define a recovery list or authorize flashing.
