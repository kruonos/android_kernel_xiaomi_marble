/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal auxiliary-bus compatibility shim for the guarded Gunyah AVF import.
 * Bouquet 5.10 does not ship the generic auxiliary bus. The modern donor
 * Gunyah RM driver only uses it to publish an optional child device, so these
 * stubs are sufficient for compile-only staging while the real RM integration
 * strategy is still being decided.
 */
#ifndef _LINUX_AUXILIARY_BUS_H
#define _LINUX_AUXILIARY_BUS_H

#include <linux/device.h>

struct auxiliary_device {
	struct device dev;
	const char *name;
	u32 id;
};

static inline int auxiliary_device_init(struct auxiliary_device *auxdev)
{
	return 0;
}

static inline int auxiliary_device_add(struct auxiliary_device *auxdev)
{
	return 0;
}

static inline void auxiliary_device_uninit(struct auxiliary_device *auxdev)
{
}

#endif /* _LINUX_AUXILIARY_BUS_H */
