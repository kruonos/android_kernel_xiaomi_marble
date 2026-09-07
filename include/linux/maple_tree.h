/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal xarray-backed Maple Tree compatibility shim for the experimental
 * Gunyah AVF backport. It intentionally implements only the small API surface
 * used by drivers/virt/gunyah/avf/ and is not a replacement for the Linux 6.x
 * Maple Tree core MM conversion.
 */
#ifndef _LINUX_MAPLE_TREE_H
#define _LINUX_MAPLE_TREE_H

#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/types.h>
#include <linux/xarray.h>

struct maple_tree {
	struct xarray xa;
};

static inline void mt_init(struct maple_tree *mt)
{
	xa_init(&mt->xa);
}

static inline int mtree_insert_range(struct maple_tree *mt,
				     unsigned long first, unsigned long last,
				     void *entry, gfp_t gfp)
{
	unsigned long index = first;
	void *old;

	if (last < first)
		return -EINVAL;

	old = xa_find(&mt->xa, &index, last, XA_PRESENT);
	if (old)
		return -EEXIST;

	old = xa_store_range(&mt->xa, first, last, entry, gfp);
	return xa_err(old);
}

static inline void *mtree_erase(struct maple_tree *mt, unsigned long index)
{
	return xa_erase(&mt->xa, index);
}

static inline void *mtree_load(struct maple_tree *mt, unsigned long index)
{
	return xa_load(&mt->xa, index);
}

static inline bool mtree_empty(const struct maple_tree *mt)
{
	return xa_empty(&mt->xa);
}

static inline void mtree_destroy(struct maple_tree *mt)
{
	xa_destroy(&mt->xa);
}

static inline void *mt_find_after(struct maple_tree *mt, unsigned long *index,
					 unsigned long max)
{
	return xa_find_after(&mt->xa, index, max, XA_PRESENT);
}

#define mt_for_each(mt, entry, index, max) \
	xa_for_each_range(&(mt)->xa, index, entry, index, max)

#endif /* _LINUX_MAPLE_TREE_H */
