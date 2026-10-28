/*
 * Copyright (C) 2012-2014 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*
 * Linux 4.9 port: Mali sync flags are backed by struct fence, fds by
 * struct sync_file (drivers/dma-buf).
 */

#include "mali_sync.h"

#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/string.h>

#include "mali_osk.h"
#include "mali_kernel_common.h"

struct mali_sync_flag {
	struct fence fence;             /* must stay first */
	spinlock_t lock;
	struct sync_timeline *sync_tl;  /* holds a timeline reference */
	u32 point;
};

static inline struct mali_sync_flag *to_mali_sync_flag(struct fence *fence)
{
	return container_of(fence, struct mali_sync_flag, fence);
}

/* ---- timeline ---------------------------------------------------------- */

static void mali_sync_timeline_free(struct kref *kref)
{
	kfree(container_of(kref, struct sync_timeline, kref));
}

struct sync_timeline *mali_sync_timeline_create(const char *name)
{
	struct sync_timeline *sync_tl;

	sync_tl = kzalloc(sizeof(*sync_tl), GFP_KERNEL);
	if (NULL == sync_tl) return NULL;

	kref_init(&sync_tl->kref);
	sync_tl->context = fence_context_alloc(1);
	strlcpy(sync_tl->name, name ? name : "mali", sizeof(sync_tl->name));

	return sync_tl;
}

void mali_sync_timeline_destroy(struct sync_timeline *sync_tl)
{
	if (NULL != sync_tl)
		kref_put(&sync_tl->kref, mali_sync_timeline_free);
}

mali_bool mali_sync_timeline_is_ours(struct sync_timeline *sync_tl)
{
	/* struct sync_timeline is private to Mali on this kernel. */
	return (NULL != sync_tl) ? MALI_TRUE : MALI_FALSE;
}

/* ---- fence ops ----------------------------------------------------------- */

static const char *mali_sync_fence_get_driver_name(struct fence *fence)
{
	return "mali";
}

static const char *mali_sync_fence_get_timeline_name(struct fence *fence)
{
	return to_mali_sync_flag(fence)->sync_tl->name;
}

static bool mali_sync_fence_enable_signaling(struct fence *fence)
{
	/* Mali always calls fence_signal() explicitly. */
	return true;
}

static void mali_sync_fence_release(struct fence *fence)
{
	struct mali_sync_flag *flag = to_mali_sync_flag(fence);

	mali_sync_timeline_destroy(flag->sync_tl);
	kfree_rcu(flag, fence.rcu);
}

static void mali_sync_fence_value_str(struct fence *fence, char *str, int size)
{
	snprintf(str, size, "%u", to_mali_sync_flag(fence)->point);
}

static const struct fence_ops mali_sync_fence_ops = {
	.get_driver_name   = mali_sync_fence_get_driver_name,
	.get_timeline_name = mali_sync_fence_get_timeline_name,
	.enable_signaling  = mali_sync_fence_enable_signaling,
	.wait              = fence_default_wait,
	.release           = mali_sync_fence_release,
	.fence_value_str   = mali_sync_fence_value_str,
};

/* ---- sync flag ------------------------------------------------------------ */

struct mali_sync_flag *mali_sync_flag_create(struct sync_timeline *sync_tl, u32 point)
{
	struct mali_sync_flag *flag;

	if (NULL == sync_tl) return NULL;

	flag = kzalloc(sizeof(*flag), GFP_KERNEL);
	if (NULL == flag) return NULL;

	spin_lock_init(&flag->lock);
	kref_get(&sync_tl->kref);
	flag->sync_tl = sync_tl;
	flag->point = point;
	fence_init(&flag->fence, &mali_sync_fence_ops, &flag->lock, sync_tl->context, point);

	return flag;
}

void mali_sync_flag_get(struct mali_sync_flag *flag)
{
	MALI_DEBUG_ASSERT_POINTER(flag);
	fence_get(&flag->fence);
}

void mali_sync_flag_put(struct mali_sync_flag *flag)
{
	MALI_DEBUG_ASSERT_POINTER(flag);
	fence_put(&flag->fence);
}

void mali_sync_flag_signal(struct mali_sync_flag *flag, int error)
{
	MALI_DEBUG_ASSERT_POINTER(flag);

	if (0 > error)
		fence_set_error(&flag->fence, error);
	fence_signal(&flag->fence);
}

struct fence *mali_sync_flag_create_fence(struct mali_sync_flag *flag)
{
	MALI_DEBUG_ASSERT_POINTER(flag);
	return fence_get(&flag->fence);
}

struct fence *mali_sync_timeline_create_signaled_fence(struct sync_timeline *sync_tl)
{
	struct mali_sync_flag *flag;
	struct fence *fence;

	MALI_DEBUG_ASSERT_POINTER(sync_tl);

	flag = mali_sync_flag_create(sync_tl, 0);
	if (NULL == flag) return NULL;

	fence = mali_sync_flag_create_fence(flag);
	mali_sync_flag_signal(flag, 0);
	mali_sync_flag_put(flag);

	return fence;
}

/* ---- sync file / merge ------------------------------------------------------ */

s32 mali_sync_fence_fd_alloc(struct fence *sync_fence)
{
	struct sync_file *sync_file;
	s32 fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fence_put(sync_fence);
		return -1;
	}

	/* sync_file_create() takes its own fence reference. */
	sync_file = sync_file_create(sync_fence);
	fence_put(sync_fence);
	if (NULL == sync_file) {
		put_unused_fd(fd);
		return -1;
	}

	fd_install(fd, sync_file->file);
	return fd;
}

static unsigned int mali_sync_fence_count(struct fence *fence)
{
	return fence_is_array(fence) ? to_fence_array(fence)->num_fences : 1;
}

static unsigned int mali_sync_fence_collect(struct fence **fences, unsigned int n, struct fence *fence)
{
	if (fence_is_array(fence)) {
		struct fence_array *array = to_fence_array(fence);
		unsigned int i;

		for (i = 0; i < array->num_fences; i++)
			fences[n++] = fence_get(array->fences[i]);
	} else {
		fences[n++] = fence_get(fence);
	}
	return n;
}

struct fence *mali_sync_fence_merge(struct fence *sync_fence1, struct fence *sync_fence2)
{
	struct fence_array *array = NULL;
	struct fence **fences;
	unsigned int num, n = 0, i;

	MALI_DEBUG_ASSERT_POINTER(sync_fence1);
	MALI_DEBUG_ASSERT_POINTER(sync_fence2);

	num = mali_sync_fence_count(sync_fence1) + mali_sync_fence_count(sync_fence2);
	fences = kcalloc(num, sizeof(*fences), GFP_KERNEL);
	if (NULL != fences) {
		n = mali_sync_fence_collect(fences, n, sync_fence1);
		n = mali_sync_fence_collect(fences, n, sync_fence2);

		/* On success the array owns fences[] and its references. */
		array = fence_array_create(n, fences, fence_context_alloc(1), 1, false);
		if (NULL == array) {
			for (i = 0; i < n; i++)
				fence_put(fences[i]);
			kfree(fences);
		}
	}

	fence_put(sync_fence1);
	fence_put(sync_fence2);

	return (NULL != array) ? &array->base : NULL;
}

/* ---- status / async wait -------------------------------------------------------- */

static int mali_sync_fence_status_single(struct fence *fence)
{
	if (!fence_is_signaled(fence))
		return 0;
	return fence->error ? fence->error : 1;
}

/*
 * 0 = active, 1 = signaled, <0 = signaled with error.  Does not take
 * fence->lock itself (fence_is_signaled() only does so when it ends up
 * signaling the fence), so it is safe under the Mali timeline lock.
 * fence_array's own ->signaled() only works after enable_signaling, so look
 * at the components directly.
 */
int mali_sync_fence_status(struct fence *fence)
{
	if (fence_is_array(fence)) {
		struct fence_array *array = to_fence_array(fence);
		int err = 0;
		unsigned int i;

		for (i = 0; i < array->num_fences; i++) {
			int status = mali_sync_fence_status_single(array->fences[i]);

			if (0 == status)
				return 0;
			if (0 > status)
				err = status;
		}
		return err ? err : 1;
	}

	return mali_sync_fence_status_single(fence);
}

static void mali_sync_fence_waiter_work(struct work_struct *work)
{
	struct sync_fence_waiter *waiter = container_of(work, struct sync_fence_waiter, work);

	/* May free the waiter (and drop the last fence reference). */
	waiter->callback(waiter->fence, waiter);
}

static void mali_sync_fence_waiter_cb(struct fence *fence, struct fence_cb *cb)
{
	struct sync_fence_waiter *waiter = container_of(cb, struct sync_fence_waiter, cb);

	/* Called with fence->lock held: defer the Mali callback. */
	queue_work(system_highpri_wq, &waiter->work);
}

int mali_sync_fence_wait_async(struct fence *fence, struct sync_fence_waiter *waiter)
{
	int err;

	waiter->fence = fence;
	INIT_WORK(&waiter->work, mali_sync_fence_waiter_work);

	err = fence_add_callback(fence, &waiter->cb, mali_sync_fence_waiter_cb);
	if (-ENOENT == err) {
		/* Already signaled. */
		int status = mali_sync_fence_status(fence);

		return (0 > status) ? status : 1;
	}

	return err;
}

int mali_sync_fence_cancel_async(struct fence *fence, struct sync_fence_waiter *waiter)
{
	if (fence_remove_callback(fence, &waiter->cb))
		return 0;

	/* Callback already fired; the deferred work will (or did) run it. */
	return -ENOENT;
}
