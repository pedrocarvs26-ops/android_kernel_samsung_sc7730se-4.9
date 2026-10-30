/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Spreadtrum MM IOMMU (drivers/misc/sprd_iommu.c), used by ION for the
 * vendor ION_SPRD_CUSTOM_MM_MAP and MM_UNMAP commands.
 */
#ifndef _LINUX_SPRD_IOMMU_H
#define _LINUX_SPRD_IOMMU_H

#include <linux/errno.h>
#include <linux/types.h>

struct sg_table;

#ifdef CONFIG_SPRD_IOMMU
int sprd_iommu_mm_map(struct sg_table *table, size_t size,
		      unsigned long *iova);
void sprd_iommu_mm_unmap(unsigned long iova, size_t size);
#else
static inline int sprd_iommu_mm_map(struct sg_table *table, size_t size,
				    unsigned long *iova)
{
	return -ENODEV;
}

static inline void sprd_iommu_mm_unmap(unsigned long iova, size_t size)
{
}
#endif

#endif /* _LINUX_SPRD_IOMMU_H */
