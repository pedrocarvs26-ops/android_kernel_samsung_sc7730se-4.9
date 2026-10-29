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
 *   FENCE  - primary/virtual HWC software timelines exported as sync_files
 *   others - -ENOTTY (no GSP/MM IOMMU yet)
 */

#define pr_fmt(fmt) "sprd-ion: " fmt

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
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

struct ion_fence_data {
	__u32 device_type;
	__s32 life_value;
	__s32 release_fence_fd;
	__s32 retired_fence_fd;
};

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

static int __init sprd_ion_init(void)
{
	int i, nr = 0;

	/* Initialize before ion_device_create publishes /dev/ion. */
	for (i = 0; i < ARRAY_SIZE(sprd_timelines); i++) {
		struct sprd_fence_timeline *timeline = &sprd_timelines[i];

		mutex_init(&timeline->mutex);
		spin_lock_init(&timeline->lock);
		INIT_LIST_HEAD(&timeline->pending);
		timeline->context = fence_context_alloc(1);
	}

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
	pr_info("display sync timelines: primary/virtual (sync_file)\n");
	return 0;
}
device_initcall(sprd_ion_init);
