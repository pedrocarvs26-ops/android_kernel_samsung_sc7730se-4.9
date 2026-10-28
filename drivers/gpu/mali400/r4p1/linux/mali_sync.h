/*
 * Copyright (C) 2012-2014 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/**
 * @file mali_sync.h
 *
 * Mali interface for Linux sync objects.
 *
 * Linux 4.9 port: the old Android sync framework (sync_timeline/sync_pt/
 * sync_fence) is gone.  Mali sync flags are now real struct fence objects and
 * fds are struct sync_file.  A small compat layer below keeps the common
 * timeline code mostly untouched.
 */

#ifndef _MALI_SYNC_H_
#define _MALI_SYNC_H_

#if defined(MALI_HAVE_SYNC)

#include <linux/seq_file.h>
#include <linux/version.h>
#include <linux/kref.h>
#include <linux/workqueue.h>
#include <linux/fence.h>
#include <linux/fence-array.h>
#include <linux/sync_file.h>

#include "mali_osk.h"

struct mali_sync_flag;

/* Minimal stand-in for the old Android sync timeline. */
struct sync_timeline {
	struct kref kref;
	u64 context;
	char name[32];
};

struct sync_fence_waiter;
typedef void (*sync_callback_t)(struct fence *fence, struct sync_fence_waiter *waiter);

/*
 * Async fence waiter.  The Mali callback is never run from inside
 * fence_signal() (which holds fence->lock): it is bounced to a workqueue so
 * the callback may take the Mali timeline system lock without creating an
 * AB-BA with fence_add_callback()/fence_remove_callback(), which Mali calls
 * while holding that lock.
 */
struct sync_fence_waiter {
	struct fence_cb cb;
	struct work_struct work;
	struct fence *fence;
	sync_callback_t callback;
};

static inline void sync_fence_waiter_init(struct sync_fence_waiter *waiter, sync_callback_t callback)
{
	waiter->callback = callback;
	waiter->fence = NULL;
}

int mali_sync_fence_wait_async(struct fence *fence, struct sync_fence_waiter *waiter);
int mali_sync_fence_cancel_async(struct fence *fence, struct sync_fence_waiter *waiter);
int mali_sync_fence_status(struct fence *fence);
void mali_sync_timeline_destroy(struct sync_timeline *sync_tl);

/* 0 = waiting, 1 = already signaled, <0 = already signaled with error. */
static inline int sync_fence_wait_async(struct fence *fence, struct sync_fence_waiter *waiter)
{
	return mali_sync_fence_wait_async(fence, waiter);
}

/* 0 = removed (callback will not run), -ENOENT = callback ran or is queued. */
static inline int sync_fence_cancel_async(struct fence *fence, struct sync_fence_waiter *waiter)
{
	return mali_sync_fence_cancel_async(fence, waiter);
}

static inline struct fence *sync_fence_fdget(int fd)
{
	return sync_file_get_fence(fd);
}

static inline void sync_fence_put(struct fence *fence)
{
	fence_put(fence);
}

static inline void sync_timeline_destroy(struct sync_timeline *sync_tl)
{
	mali_sync_timeline_destroy(sync_tl);
}

/**
 * Create a sync timeline.
 *
 * @param name Name of the sync timeline.
 * @return The new sync timeline if successful, NULL if not.
 */
struct sync_timeline *mali_sync_timeline_create(const char *name);

/**
 * Check if sync timeline belongs to Mali.
 *
 * @param sync_tl Sync timeline to check.
 * @return MALI_TRUE if sync timeline belongs to Mali, MALI_FALSE if not.
 */
mali_bool mali_sync_timeline_is_ours(struct sync_timeline *sync_tl);

/**
 * Creates a file descriptor representing the sync fence.  Will release sync fence if allocation of
 * file descriptor fails.
 *
 * @param sync_fence Sync fence.
 * @return File descriptor representing sync fence if successful, or -1 if not.
 */
s32 mali_sync_fence_fd_alloc(struct fence *sync_fence);

/**
 * Merges two sync fences.  Both input sync fences will be released.
 *
 * @param sync_fence1 First sync fence.
 * @param sync_fence2 Second sync fence.
 * @return New sync fence that is the result of the merger if successful, or NULL if not.
 */
struct fence *mali_sync_fence_merge(struct fence *sync_fence1, struct fence *sync_fence2);

/**
 * Create a sync fence that is already signaled.
 *
 * @param tl Sync timeline.
 * @return New signaled sync fence if successful, NULL if not.
 */
struct fence *mali_sync_timeline_create_signaled_fence(struct sync_timeline *sync_tl);

/**
 * Create a sync flag.
 *
 * @param sync_tl Sync timeline.
 * @param point Point on Mali timeline.
 * @return New sync flag if successful, NULL if not.
 */
struct mali_sync_flag *mali_sync_flag_create(struct sync_timeline *sync_tl, u32 point);

/**
 * Grab sync flag reference.
 *
 * @param flag Sync flag.
 */
void mali_sync_flag_get(struct mali_sync_flag *flag);

/**
 * Release sync flag reference.  If this was the last reference, the sync flag will be freed.
 *
 * @param flag Sync flag.
 */
void mali_sync_flag_put(struct mali_sync_flag *flag);

/**
 * Signal sync flag.  All sync fences created from this flag will be signaled.
 *
 * @param flag Sync flag to signal.
 * @param error Negative error code, or 0 if no error.
 */
void mali_sync_flag_signal(struct mali_sync_flag *flag, int error);

/**
 * Create a sync fence attached to given sync flag.
 *
 * @param flag Sync flag.
 * @return New sync fence if successful, NULL if not.
 */
struct fence *mali_sync_flag_create_fence(struct mali_sync_flag *flag);

#endif /* defined(MALI_HAVE_SYNC) */

#endif /* _MALI_SYNC_H_ */
