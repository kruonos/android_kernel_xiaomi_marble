/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal compatibility helpers for compiling selected 6.x-style code on the
 * Bouquet 5.10 base. This is intentionally small and only covers the helpers
 * needed by the experimental Gunyah AVF VM-manager import.
 */
#ifndef _LINUX_FOLIO_COMPAT_H
#define _LINUX_FOLIO_COMPAT_H

#include <linux/mm.h>
#include <linux/pagemap.h>

#ifndef HAVE_BOUQUET_FOLIO_COMPAT
#define HAVE_BOUQUET_FOLIO_COMPAT

struct folio;

static inline struct page *folio_page(struct folio *folio, unsigned long n)
{
	return ((struct page *)folio) + n;
}

static inline struct folio *page_folio(struct page *page)
{
	return (struct folio *)compound_head(page);
}

static inline struct folio *pfn_folio(unsigned long pfn)
{
	return page_folio(pfn_to_page(pfn));
}

static inline unsigned long folio_pfn(struct folio *folio)
{
	return page_to_pfn(folio_page(folio, 0));
}

static inline unsigned int folio_nr_pages(struct folio *folio)
{
	return compound_nr(folio_page(folio, 0));
}

static inline size_t folio_size(struct folio *folio)
{
	return PAGE_SIZE * folio_nr_pages(folio);
}

static inline pgoff_t folio_index(struct folio *folio)
{
	return page_index(folio_page(folio, 0));
}

static inline unsigned long folio_page_idx(struct folio *folio, struct page *page)
{
	return page - folio_page(folio, 0);
}

static inline bool folio_mapped(struct folio *folio)
{
	return page_mapped(folio_page(folio, 0));
}

static inline bool folio_test_private(struct folio *folio)
{
	return PagePrivate(folio_page(folio, 0));
}

static inline void folio_get(struct folio *folio)
{
	get_page(folio_page(folio, 0));
}

static inline void folio_put(struct folio *folio)
{
	put_page(folio_page(folio, 0));
}

static inline void folio_lock(struct folio *folio)
{
	lock_page(folio_page(folio, 0));
}

static inline void folio_unlock(struct folio *folio)
{
	unlock_page(folio_page(folio, 0));
}

#endif /* HAVE_BOUQUET_FOLIO_COMPAT */
#endif /* _LINUX_FOLIO_COMPAT_H */
