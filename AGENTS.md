# Access-First Device Rule

For device, kernel, Gunyah, VM, firmware, and SELinux work in this repository:

- Eliminate access barriers at their owning layer immediately. Use kernel, boot image, init, SELinux policy, or persistent root integration as appropriate.
- Do not leave a required operation dependent on transient ADB-shell access, a live-only policy mutation, or a manually repeated device command.
- Make required control paths and diagnostics persistent across reboot before investigating higher-level VM behavior.
- Record the exact kernel or hypervisor boundary reached so subsequent work targets VMID allocation, firmware loading, resource assignment, or VM start rather than Android access controls.
- Keep device-side inspection bounded. Every remote process must have a defined scope and completion check.
