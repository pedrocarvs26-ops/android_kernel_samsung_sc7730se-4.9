// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC8830-family ION glue for the 4.9 staging ION core.
 *
 * The Sprd Android userspace (gralloc.sc8830, libion_sprd, SPRDHWComposer,
 * the OMX components) addresses heaps by the fixed ids of the 3.10 vendor
 * kernel (include/video/ion_sprd.h there):
 *
 *   1  ION_HEAP_ID_SYSTEM   ordinary graphics buffers
 *   2  ION_HEAP_ID_MM       video / camera
 *   3  ION_HEAP_ID_OVERLAY  HWC overlay planes (a 16 MiB carveout on 3.10)
 *
 * The generic dummy ION driver has a physically contiguous heap at id 1, so
 * every gralloc buffer came from contiguous kmalloc memory and anything big
 * (the 1920x1920 wallpaper, 14 MiB) failed with -ENOMEM.  Nothing that needs
 * contiguous memory (DISPC overlays, GSP, VSP, ISP) is driven by this kernel
 * yet and the Mali-400 has its own MMU, so all three ids are backed by the
 * page-pool system heap for now.
 *
 * Vendor ION_IOC_CUSTOM commands:
 *   PHYS   - answered only for buffers that are a single physical chunk
 *   MSYNC  - whole-buffer clean (a superset of the 3.10 range flush)
 *   others - -ENOTTY (no GSP/MM IOMMU and no Sprd display fences yet)
 */

#define pr_fmt(fmt) "sprd-ion: " fmt

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include "ion.h"
#include "ion_priv.h"

#define ION_HEAP_ID_SYSTEM	1
#define ION_HEAP_ID_MM		2
#define ION_HEAP_ID_OVERLAY	3

/* ABI of the 3.10 vendor kernel, include/video/ion_sprd.h */
enum ion_sprd_custom_cmd {
	ION_SPRD_CUSTOM_PHYS,
	ION_SPRD_CUSTOM_MSYNC,
	ION_SPRD_CUSTOM_GSP_MAP,
	ION_SPRD_CUSTOM_GSP_UNMAP,
	ION_SPRD_CUSTOM_MM_MAP,
	ION_SPRD_CUSTOM_MM_UNMAP,
	ION_SPRD_CUSTOM_FENCE_CREATE,
	ION_SPRD_CUSTOM_FENCE_SIGNAL,
	ION_SPRD_CUSTOM_FENCE_DUP,
};

struct ion_phys_data {
	int fd_buffer;
	unsigned long phys;
	size_t size;
};

struct ion_msync_data {
	int fd_buffer;
	void __user *vaddr;
	void __user *paddr;
	size_t size;
};

static struct ion_platform_heap sprd_ion_heaps[] = {
	{
		.id	= ION_HEAP_ID_SYSTEM,
		.type	= ION_HEAP_TYPE_SYSTEM,
		.name	= "ion_heap_system",
	},
	{
		.id	= ION_HEAP_ID_MM,
		.type	= ION_HEAP_TYPE_SYSTEM,
		.name	= "ion_heap_mm",
	},
	{
		.id	= ION_HEAP_ID_OVERLAY,
		.type	= ION_HEAP_TYPE_SYSTEM,
		.name	= "ion_heap_overlay",
	},
};

static struct ion_device *sprd_idev;
static unsigned long sprd_ion_warned;

static long sprd_ion_custom_ioctl(struct ion_client *client, unsigned int cmd,
				  unsigned long arg)
{
	switch (cmd) {
	case ION_SPRD_CUSTOM_PHYS: {
		struct ion_phys_data data;
		phys_addr_t phys;
		size_t size;
		int ret;

		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		ret = ion_sprd_buffer_phys(data.fd_buffer, &phys, &size);
		if (ret)
			return ret;
		data.phys = phys;
		data.size = size;
		if (copy_to_user((void __user *)arg, &data, sizeof(data)))
			return -EFAULT;
		return 0;
	}
	case ION_SPRD_CUSTOM_MSYNC: {
		struct ion_msync_data data;

		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		return ion_sync_for_device(client, data.fd_buffer);
	}
	default:
		if (cmd < BITS_PER_LONG && !test_and_set_bit(cmd, &sprd_ion_warned))
			pr_info("custom cmd %u not supported\n", cmd);
		return -ENOTTY;
	}
}

static int __init sprd_ion_init(void)
{
	int i, nr = 0;

	sprd_idev = ion_device_create(sprd_ion_custom_ioctl);
	if (IS_ERR(sprd_idev))
		return PTR_ERR(sprd_idev);

	for (i = 0; i < ARRAY_SIZE(sprd_ion_heaps); i++) {
		struct ion_heap *heap = ion_heap_create(&sprd_ion_heaps[i]);

		if (IS_ERR_OR_NULL(heap)) {
			pr_err("%s: create failed (%ld)\n",
			       sprd_ion_heaps[i].name, PTR_ERR(heap));
			continue;
		}
		ion_device_add_heap(sprd_idev, heap);
		nr++;
	}
	pr_info("%d heaps: ids 1/2/3 (system/mm/overlay) backed by system pages\n",
		nr);
	return 0;
}
device_initcall(sprd_ion_init);
