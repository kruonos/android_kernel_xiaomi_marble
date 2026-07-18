# SPDX-License-Identifier: GPL-2.0-only
# Modern /dev/gunyah AVF/crosvm VM-manager import from SM8750 6.6.
# This directory is intentionally gated behind CONFIG_GUNYAH_AVF_EXPERIMENTAL
# and is not part of the bootable Bouquet baseline yet.

gunyah_rsc_mgr-y += rsc_mgr.o rsc_mgr_rpc.o vm_mgr.o vm_mgr_mem.o

obj-$(CONFIG_GUNYAH_AVF_EXPERIMENTAL) += gunyah.o gunyah_rsc_mgr.o gunyah_vcpu.o \
	gunyah_irqfd.o gunyah_ioeventfd.o
