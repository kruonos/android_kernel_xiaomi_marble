# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_GH_ARM64_DRV)	 += gh_arm_drv.o
gh_arm_drv-y			:= gh_arm.o irq.o reset.o

# Modern Gunyah hypercall glue for the experimental crosvm/AVF VM-manager path.
obj-$(CONFIG_GUNYAH_AVF_EXPERIMENTAL) += gunyah_hypercall.o
