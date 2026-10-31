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
 * (the 1920x1920 wallpaper, 14 MiB) failed with -ENOMEM.  The Mali-400 has
 * its own MMU and no DISPC overlay or GSP is driven yet, so ids 1 and 3 are
 * backed by the page-pool system heap.  id 2 takes physically contiguous
 * buffers from the CMA pool named by the ion node's memory-region, or system
 * pages if there is none, for the VSP and JPEG codecs when they work without
 * the MM IOMMU.
 *
 * Vendor ION_IOC_CUSTOM commands:
 *   PHYS   - answered only for buffers that are a single physical chunk
 *   MSYNC  - whole-buffer clean + invalidate (3.10 flushed the range)
 *   MM_MAP/MM_UNMAP - counted MM IOMMU mappings (CONFIG_SPRD_IOMMU)
 *   FENCE  - primary/virtual HWC software timelines exported as sync_files
 *   others - -ENOTTY (no GSP IOMMU)
 */

#define pr_fmt(fmt) "sprd-ion: " fmt

#include <linux/bitops.h>
#include <linux/dma-contiguous.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>
#include <video/ion_sprd.h>
#include "ion.h"
#include "ion_priv.h"

/* These timelines live for the lifetime of this built-in ION device. */
struct sprd_fence_timeline {
	/* Serializes fence pairs against timeline increments. */
	struct mutex mutex;
	/* Protects value and pending; also used as each fence's lock. */
	spinlock_t lock;
	struct list_head pending;
	u64 context;
	u32 value;
	const char *name;
};

struct sprd_fence {
	struct fence base;
	struct list_head link;
	struct sprd_fence_timeline *timeline;
};

static struct sprd_fence_timeline sprd_timelines[] = {
	{ .name = "sprd-timeline" },
	{ .name = "sprd-timeline-virtual" },
};

static struct sprd_fence *to_sprd_fence(struct fence *fence)
{
	return container_of(fence, struct sprd_fence, base);
}

static const char *sprd_fence_driver_name(struct fence *fence)
{
	return "sprd-ion";
}

static const char *sprd_fence_timeline_name(struct fence *fence)
{
	return to_sprd_fence(fence)->timeline->name;
}

static bool sprd_fence_signaled(struct fence *fence)
{
	struct sprd_fence_timeline *timeline = to_sprd_fence(fence)->timeline;

	return !__fence_is_later(fence->seqno, READ_ONCE(timeline->value));
}

static bool sprd_fence_enable_signaling(struct fence *fence)
{
	return !sprd_fence_signaled(fence);
}

static void sprd_fence_release(struct fence *fence)
{
	struct sprd_fence *pt = to_sprd_fence(fence);
	unsigned long flags;

	/* Only a still-pending point needs the lock to unlink. */
	if (!list_empty(&pt->link)) {
		spin_lock_irqsave(fence->lock, flags);
		if (!list_empty(&pt->link))
			list_del_init(&pt->link);
		spin_unlock_irqrestore(fence->lock, flags);
	}
	fence_free(fence);
}

static void sprd_fence_value_str(struct fence *fence, char *str, int size)
{
	snprintf(str, size, "%u", fence->seqno);
}

static void sprd_fence_timeline_value_str(struct fence *fence,
					  char *str, int size)
{
	struct sprd_fence_timeline *timeline = to_sprd_fence(fence)->timeline;

	snprintf(str, size, "%u", READ_ONCE(timeline->value));
}

static const struct fence_ops sprd_fence_ops = {
	.get_driver_name = sprd_fence_driver_name,
	.get_timeline_name = sprd_fence_timeline_name,
	.enable_signaling = sprd_fence_enable_signaling,
	.signaled = sprd_fence_signaled,
	.wait = fence_default_wait,
	.release = sprd_fence_release,
	.fence_value_str = sprd_fence_value_str,
	.timeline_value_str = sprd_fence_timeline_value_str,
};

/* Caller holds timeline->mutex across both points of a release/retire pair. */
static struct sync_file *sprd_fence_create(struct sprd_fence_timeline *timeline,
					   u32 value, const char *name)
{
	struct sprd_fence *pt;
	struct sync_file *sync_file;
	unsigned long flags;

	pt = kzalloc(sizeof(*pt), GFP_KERNEL);
	if (!pt)
		return NULL;

	pt->timeline = timeline;
	INIT_LIST_HEAD(&pt->link);
	fence_init(&pt->base, &sprd_fence_ops, &timeline->lock,
		   timeline->context, value);

	spin_lock_irqsave(&timeline->lock, flags);
	if (sprd_fence_signaled(&pt->base))
		fence_signal_locked(&pt->base);
	else
		list_add_tail(&pt->link, &timeline->pending);
	spin_unlock_irqrestore(&timeline->lock, flags);

	sync_file = sync_file_create(&pt->base);
	/* sync_file_create takes its own ref; failure must unlink the point. */
	fence_put(&pt->base);
	if (sync_file)
		strlcpy(sync_file->name, name, sizeof(sync_file->name));
	return sync_file;
}

static long sprd_ion_fence_create(unsigned long arg)
{
	struct ion_fence_data data;
	struct sprd_fence_timeline *timeline;
	struct sync_file *release, *retired;
	int release_fd, retired_fd, ret;
	u32 value;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;
	if (data.device_type >= ARRAY_SIZE(sprd_timelines))
		return -EFAULT;
	timeline = &sprd_timelines[data.device_type];

	release_fd = get_unused_fd_flags(O_CLOEXEC);
	if (release_fd < 0)
		return release_fd;
	retired_fd = get_unused_fd_flags(O_CLOEXEC);
	if (retired_fd < 0) {
		ret = retired_fd;
		goto err_release_fd;
	}

	mutex_lock(&timeline->mutex);
	/* Unsigned seqnos and wrap-safe comparisons handle counter wrap. */
	value = timeline->value + (u32)data.life_value;
	release = sprd_fence_create(timeline, value, "HWCRelease");
	retired = NULL;
	if (release)
		retired = sprd_fence_create(timeline, value, "HWCRetire");
	mutex_unlock(&timeline->mutex);
	ret = -ENOMEM;
	if (!release || !retired)
		goto err_files;

	data.release_fence_fd = release_fd;
	data.retired_fence_fd = retired_fd;
	ret = -EFAULT;
	if (copy_to_user((void __user *)arg, &data, sizeof(data)))
		goto err_files;

	/* Install after copy succeeds; error paths never close live fds. */
	fd_install(release_fd, release->file);
	fd_install(retired_fd, retired->file);
	return 0;

err_files:
	if (retired)
		fput(retired->file);
	if (release)
		fput(release->file);
	put_unused_fd(retired_fd);
err_release_fd:
	put_unused_fd(release_fd);
	return ret;
}

static long sprd_ion_fence_signal(unsigned long arg)
{
	struct ion_fence_data data;
	struct sprd_fence_timeline *timeline;
	struct sprd_fence *pt, *next;
	unsigned long flags;
	LIST_HEAD(signaled);

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;
	if (data.device_type >= ARRAY_SIZE(sprd_timelines))
		return -EFAULT;
	timeline = &sprd_timelines[data.device_type];

	mutex_lock(&timeline->mutex);
	spin_lock_irqsave(&timeline->lock, flags);
	timeline->value++;
	list_for_each_entry_safe(pt, next, &timeline->pending, link) {
		if (!sprd_fence_signaled(&pt->base))
			continue;
		/* A point already being released only needs unlinking. */
		if (!fence_get_rcu(&pt->base)) {
			list_del_init(&pt->link);
			continue;
		}
		/* Our ref keeps callbacks from freeing it under the lock. */
		list_move_tail(&pt->link, &signaled);
		fence_signal_locked(&pt->base);
	}
	spin_unlock_irqrestore(&timeline->lock, flags);
	mutex_unlock(&timeline->mutex);

	/* Drop those refs unlocked: they may be the last ones. */
	list_for_each_entry_safe(pt, next, &signaled, link) {
		list_del_init(&pt->link);
		fence_put(&pt->base);
	}
	return 0;
}

/*
 * Unlike the 4.9 ion_cma_heap this keeps the pages in the linear map and
 * honours ION_FLAG_CACHED: gralloc allocates the video buffers that it
 * also lets the CPU touch cached, and OMX syncs them with MSYNC.  The pool
 * is "reusable", so movable pages live there while no codec needs it.
 */
struct sprd_ion_cma_heap {
	struct ion_heap heap;
	struct device *dev;
};

static struct device *sprd_ion_cma_dev(struct ion_heap *heap)
{
	return container_of(heap, struct sprd_ion_cma_heap, heap)->dev;
}

/* Write the zeroes back and drop the lines: user maps may be uncached. */
static void sprd_ion_cma_flush(struct page *page, size_t size)
{
	struct scatterlist sg;

	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, size, 0);
	/* ION's own convention: DMA addresses are physical on this SoC. */
	sg_dma_address(&sg) = page_to_phys(page);
	dma_sync_sg_for_device(NULL, &sg, 1, DMA_TO_DEVICE);
	dma_sync_sg_for_cpu(NULL, &sg, 1, DMA_FROM_DEVICE);
}

static int sprd_ion_cma_allocate(struct ion_heap *heap,
				 struct ion_buffer *buffer, unsigned long len,
				 unsigned long align, unsigned long flags)
{
	struct device *dev = sprd_ion_cma_dev(heap);
	unsigned long i, count = PAGE_ALIGN(len) >> PAGE_SHIFT;
	unsigned int order = align > PAGE_SIZE ? get_order(align) : 0;
	struct sg_table *table;
	struct page *page;
	int ret = -ENOMEM;

	page = dma_alloc_from_contiguous(dev, count, order);
	if (!page)
		return -ENOMEM;
	for (i = 0; i < count; i++)
		clear_highpage(nth_page(page, i));
	sprd_ion_cma_flush(page, count << PAGE_SHIFT);

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		goto err_release;
	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret)
		goto err_free;
	sg_set_page(table->sgl, page, count << PAGE_SHIFT, 0);
	buffer->sg_table = table;
	return 0;

err_free:
	kfree(table);
err_release:
	dma_release_from_contiguous(dev, page, count);
	return ret;
}

static void sprd_ion_cma_free(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->sg_table;

	dma_release_from_contiguous(sprd_ion_cma_dev(buffer->heap),
				    sg_page(table->sgl),
				    table->sgl->length >> PAGE_SHIFT);
	sg_free_table(table);
	kfree(table);
}

static struct ion_heap_ops sprd_ion_cma_ops = {
	.allocate	= sprd_ion_cma_allocate,
	.free		= sprd_ion_cma_free,
	.map_user	= ion_heap_map_user,
	.map_kernel	= ion_heap_map_kernel,
	.unmap_kernel	= ion_heap_unmap_kernel,
};

static struct ion_heap *sprd_ion_cma_heap_create(struct device *dev,
						 struct ion_platform_heap *data)
{
	struct sprd_ion_cma_heap *cma_heap;

	cma_heap = devm_kzalloc(dev, sizeof(*cma_heap), GFP_KERNEL);
	if (!cma_heap)
		return ERR_PTR(-ENOMEM);
	cma_heap->dev = dev;
	cma_heap->heap.ops = &sprd_ion_cma_ops;
	cma_heap->heap.type = ION_HEAP_TYPE_DMA;
	cma_heap->heap.id = data->id;
	cma_heap->heap.name = data->name;
	return &cma_heap->heap;
}

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
		int ret;

		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		/* Callers use it both before and after the codec's access. */
		ret = ion_sync_for_device(client, data.fd_buffer);
		if (ret)
			return ret;
		return ion_invalidate_for_cpu(client, data.fd_buffer);
	}
	case ION_SPRD_CUSTOM_GSP_MAP: {
		struct ion_mmu_data data;
		phys_addr_t phys;
		size_t size;
		int ret;

		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		ret = ion_sprd_buffer_phys(data.fd_buffer, &phys, &size);
		if (ret)
			return ret;
		data.iova_addr = (unsigned long)phys;
		data.iova_size = size;
		if (copy_to_user((void __user *)arg, &data, sizeof(data)))
			return -EFAULT;
		return 0;
	}
	case ION_SPRD_CUSTOM_GSP_UNMAP: {
		struct ion_mmu_data data;
		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		data.iova_addr = 0;
		data.iova_size = 0;
		if (copy_to_user((void __user *)arg, &data, sizeof(data)))
			return -EFAULT;
		return 0;
	}
	case ION_SPRD_CUSTOM_MM_MAP: {
		struct ion_mmu_data data;
		int ret;

		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		ret = ion_sprd_mm_map(data.fd_buffer, &data.iova_addr,
				      &data.iova_size);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
			ion_sprd_mm_unmap(data.fd_buffer);
			return -EFAULT;
		}
		return 0;
	}
	case ION_SPRD_CUSTOM_MM_UNMAP: {
		struct ion_mmu_data data;
		int ret;

		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		ret = ion_sprd_mm_unmap(data.fd_buffer);
		if (ret)
			return ret;
		/* as on 3.10, the caller's copy of the mapping is cleared */
		data.iova_addr = 0;
		data.iova_size = 0;
		if (copy_to_user((void __user *)arg, &data, sizeof(data)))
			return -EFAULT;
		return 0;
	}
	case ION_SPRD_CUSTOM_FENCE_CREATE:
		return sprd_ion_fence_create(arg);
	case ION_SPRD_CUSTOM_FENCE_SIGNAL:
		return sprd_ion_fence_signal(arg);
	case ION_SPRD_CUSTOM_FENCE_DUP:
		/* Vendor no-op: userspace duplicates with dup(). */
		return 0;
	default:
		if (cmd < BITS_PER_LONG && !test_and_set_bit(cmd, &sprd_ion_warned))
			pr_info("custom cmd %u not supported\n", cmd);
		return -ENOTTY;
	}
}

static int sprd_ion_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	bool cma;
	int i, ret, nr = 0;

	/* Initialize before ion_device_create publishes /dev/ion. */
	for (i = 0; i < ARRAY_SIZE(sprd_timelines); i++) {
		struct sprd_fence_timeline *timeline = &sprd_timelines[i];

		mutex_init(&timeline->mutex);
		spin_lock_init(&timeline->lock);
		INIT_LIST_HEAD(&timeline->pending);
		timeline->context = fence_context_alloc(1);
	}

	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV)
		dev_warn(dev, "memory-region unusable (%d)\n", ret);
	cma = !ret && dev_get_cma_area(dev);

	sprd_idev = ion_device_create(sprd_ion_custom_ioctl);
	if (IS_ERR(sprd_idev)) {
		if (!ret)
			of_reserved_mem_device_release(dev);
		return PTR_ERR(sprd_idev);
	}

	for (i = 0; i < ARRAY_SIZE(sprd_ion_heaps); i++) {
		struct ion_platform_heap *data = &sprd_ion_heaps[i];
		struct ion_heap *heap;

		if ((data->id == ION_HEAP_ID_MM || data->id == ION_HEAP_ID_OVERLAY) && cma)
			heap = sprd_ion_cma_heap_create(dev, data);
		else
			heap = ion_heap_create(data);
		if (IS_ERR_OR_NULL(heap)) {
			pr_err("%s: create failed (%ld)\n", data->name,
			       PTR_ERR(heap));
			continue;
		}
		ion_device_add_heap(sprd_idev, heap);
		nr++;
	}
	pr_info("%d heaps: id 1 (system) system pages, ids 2/3 (mm/overlay) %s\n",
		nr, cma ? "contiguous CMA" : "system pages");
	pr_info("display sync timelines: primary/virtual (sync_file)\n");
	return 0;
}

static const struct of_device_id sprd_ion_of_match[] = {
	{ .compatible = "sprd,sc8830-ion" },
	{ }
};

static struct platform_driver sprd_ion_driver = {
	.probe = sprd_ion_probe,
	.driver = {
		.name = "sprd-ion",
		.of_match_table = sprd_ion_of_match,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver(sprd_ion_driver);

int sprd_ion_get_gsp_addr(struct ion_addr_data *data)
{
	phys_addr_t phys = 0;
	size_t size = 0;
	int ret;

	if (!data)
		return -EINVAL;

	ret = ion_sprd_buffer_phys(data->fd_buffer, &phys, &size);
	if (ret) {
		pr_err("sprd_ion_get_gsp_addr: failed to get phys for fd %d (ret=%d)\n",
		       data->fd_buffer, ret);
		return ret;
	}

	data->phys_addr = (unsigned long)phys;
	data->size = size;
	data->iova_enabled = false;
	data->iova_addr = 0;
	return 0;
}
EXPORT_SYMBOL(sprd_ion_get_gsp_addr);

int sprd_ion_free_gsp_addr(int fd)
{
	return 0;
}
EXPORT_SYMBOL(sprd_ion_free_gsp_addr);
