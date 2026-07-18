/* SPDX-License-Identifier: GPL-2.0-only */
/* Compatibility helpers for the SM8750 Gunyah AVF import on Bouquet 5.10. */
#ifndef _GUNYAH_AVF_COMPAT_H
#define _GUNYAH_AVF_COMPAT_H

#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#ifndef FIELD_PREP_CONST
#define FIELD_PREP_CONST(_mask, _val) FIELD_PREP(_mask, _val)
#endif

#ifndef overflows_type
#define overflows_type(n, T) ((n) > type_max(T))
#endif

#ifndef xa_limit_16b
#define xa_limit_16b XA_LIMIT(0, USHRT_MAX)
#endif

#ifndef lower_16_bits
#define lower_16_bits(n) ((u16)(n))
#endif

#ifndef __free
#define __free(_fn)
#endif

static inline struct rb_node *gunyah_avf_rb_find(
	const void *key, const struct rb_root *tree,
	int (*cmp)(const void *key, const struct rb_node *node))
{
	struct rb_node *node = tree->rb_node;

	while (node) {
		int result = cmp(key, node);

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return node;
	}

	return NULL;
}

static inline struct rb_node *gunyah_avf_rb_find_add(
	struct rb_node *node, struct rb_root *tree,
	int (*cmp)(struct rb_node *node, const struct rb_node *parent))
{
	struct rb_node **link = &tree->rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		int result;

		parent = *link;
		result = cmp(node, parent);
		if (result < 0)
			link = &parent->rb_left;
		else if (result > 0)
			link = &parent->rb_right;
		else
			return parent;
	}

	rb_link_node(node, parent, link);
	rb_insert_color(node, tree);

	return NULL;
}

#define rb_find(key, tree, cmp) gunyah_avf_rb_find(key, tree, cmp)
#define rb_find_add(node, tree, cmp) gunyah_avf_rb_find_add(node, tree, cmp)

#endif /* _GUNYAH_AVF_COMPAT_H */
