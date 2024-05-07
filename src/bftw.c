// Copyright © Tavian Barnes <tavianator@tavianator.com>
// SPDX-License-Identifier: 0BSD

/**
 * The bftw() implementation consists of the following components:
 *
 * - struct bftw_file: A file that has been encountered during the traversal.
 *   They have reference-counted links to their parents in the directory tree.
 *
 * - struct bftw_list: A linked list of bftw_file's.
 *
 * - struct bftw_queue: A multi-stage queue of bftw_file's.
 *
 * - struct bftw_cache: An LRU list of bftw_file's with open file descriptors,
 *   used for openat() to minimize the amount of path re-traversals.
 *
 * - struct bftw_state: Represents the current state of the traversal, allowing
 *   various helper functions to take fewer parameters.
 */

#include "prelude.h"
#include "bftw.h"
#include "alloc.h"
#include "bfstd.h"
#include "diag.h"
#include "dir.h"
#include "dstring.h"
#include "ioq.h"
#include "list.h"
#include "mtab.h"
#include "stat.h"
#include "trie.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/** Initialize a bftw_stat cache. */
static void bftw_stat_init(struct bftw_stat *bufs, struct bfs_stat *stat_buf, struct bfs_stat *lstat_buf) {
	bufs->stat_buf = stat_buf;
	bufs->lstat_buf = lstat_buf;
	bufs->stat_err = -1;
	bufs->lstat_err = -1;
}

/** Fill a bftw_stat cache from another one. */
static void bftw_stat_fill(struct bftw_stat *dest, const struct bftw_stat *src) {
	if (dest->stat_err < 0 && src->stat_err >= 0) {
		dest->stat_buf = src->stat_buf;
		dest->stat_err = src->stat_err;
	}

	if (dest->lstat_err < 0 && src->lstat_err >= 0) {
		dest->lstat_buf = src->lstat_buf;
		dest->lstat_err = src->lstat_err;
	}
}

/** Cache a bfs_stat() result. */
static void bftw_stat_cache(struct bftw_stat *bufs, enum bfs_stat_flags flags, const struct bfs_stat *buf, int err) {
	if (flags & BFS_STAT_NOFOLLOW) {
		bufs->lstat_buf = buf;
		bufs->lstat_err = err;
		if (err || !S_ISLNK(buf->mode)) {
			// Non-link, so share stat info
			bufs->stat_buf = buf;
			bufs->stat_err = err;
		}
	} else if (flags & BFS_STAT_TRYFOLLOW) {
		if (err) {
			bufs->stat_err = err;
		} else if (S_ISLNK(buf->mode)) {
			bufs->lstat_buf = buf;
			bufs->lstat_err = err;
			bufs->stat_err = ENOENT;
		} else {
			bufs->stat_buf = buf;
			bufs->stat_err = err;
		}
	} else {
		bufs->stat_buf = buf;
		bufs->stat_err = err;
	}
}

/** Caching bfs_stat(). */
static const struct bfs_stat *bftw_stat_impl(struct BFTW *ftwbuf, enum bfs_stat_flags flags) {
	struct bftw_stat *bufs = &ftwbuf->stat_bufs;
	struct bfs_stat *buf;

	if (flags & BFS_STAT_NOFOLLOW) {
		buf = (struct bfs_stat *)bufs->lstat_buf;
		if (bufs->lstat_err == 0) {
			return buf;
		} else if (bufs->lstat_err > 0) {
			errno = bufs->lstat_err;
			return NULL;
		}
	} else {
		buf = (struct bfs_stat *)bufs->stat_buf;
		if (bufs->stat_err == 0) {
			return buf;
		} else if (bufs->stat_err > 0) {
			errno = bufs->stat_err;
			return NULL;
		}
	}

	struct bfs_stat *ret;
	int err;
	if (bfs_stat(ftwbuf->at_fd, ftwbuf->at_path, flags, buf) == 0) {
		ret = buf;
		err = 0;
#ifdef S_IFWHT
	} else if (errno == ENOENT && ftwbuf->type == BFS_WHT) {
		// This matches the behavior of FTS_WHITEOUT on BSD
		ret = memset(buf, 0, sizeof(*buf));
		ret->mode = S_IFWHT;
		err = 0;
#endif
	} else {
		ret = NULL;
		err = errno;
	}

	bftw_stat_cache(bufs, flags, ret, err);
	return ret;
}

const struct bfs_stat *bftw_stat(const struct BFTW *ftwbuf, enum bfs_stat_flags flags) {
	struct BFTW *mutbuf = (struct BFTW *)ftwbuf;
	const struct bfs_stat *ret;

	if (flags & BFS_STAT_TRYFOLLOW) {
		ret = bftw_stat_impl(mutbuf, BFS_STAT_FOLLOW);
		if (!ret && errno_is_like(ENOENT)) {
			ret = bftw_stat_impl(mutbuf, BFS_STAT_NOFOLLOW);
		}
	} else {
		ret = bftw_stat_impl(mutbuf, flags);
	}

	return ret;
}

const struct bfs_stat *bftw_cached_stat(const struct BFTW *ftwbuf, enum bfs_stat_flags flags) {
	const struct bftw_stat *bufs = &ftwbuf->stat_bufs;

	if (flags & BFS_STAT_NOFOLLOW) {
		if (bufs->lstat_err == 0) {
			return bufs->lstat_buf;
		}
	} else if (bufs->stat_err == 0) {
		return bufs->stat_buf;
	} else if ((flags & BFS_STAT_TRYFOLLOW) && error_is_like(bufs->stat_err, ENOENT)) {
		if (bufs->lstat_err == 0) {
			return bufs->lstat_buf;
		}
	}

	return NULL;
}

enum bfs_type bftw_type(const struct BFTW *ftwbuf, enum bfs_stat_flags flags) {
	if (flags & BFS_STAT_NOFOLLOW) {
		if (ftwbuf->type == BFS_LNK || (ftwbuf->stat_flags & BFS_STAT_NOFOLLOW)) {
			return ftwbuf->type;
		}
	} else if (flags & BFS_STAT_TRYFOLLOW) {
		if (ftwbuf->type != BFS_LNK || (ftwbuf->stat_flags & BFS_STAT_TRYFOLLOW)) {
			return ftwbuf->type;
		}
	} else {
		if (ftwbuf->type != BFS_LNK) {
			return ftwbuf->type;
		} else if (ftwbuf->stat_flags & BFS_STAT_TRYFOLLOW) {
			return BFS_ERROR;
		}
	}

	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, flags);
	if (statbuf) {
		return bfs_mode_to_type(statbuf->mode);
	} else {
		return BFS_ERROR;
	}
}

/**
 * A file.
 */
struct bftw_file {
	/** The parent directory, if any. */
	struct bftw_file *parent;
	/** The root under which this file was found. */
	struct bftw_file *root;

	/**
	 * List node for:
	 *
	 *     bftw_queue::buffer
	 *     bftw_queue::waiting
	 *     bftw_file_open()::parents
	 */
	struct bftw_file *next;

	/**
	 * List node for:
	 *
	 *     bftw_queue::ready
	 *     bftw_state::to_close
	 */
	struct { struct bftw_file *next; } ready;

	/**
	 * List node for bftw_cache.
	 */
	struct {
		struct bftw_file *prev;
		struct bftw_file *next;
	} lru;

	/** This file's depth in the walk. */
	size_t depth;
	/** Reference count (for ->parent). */
	size_t refcount;

	/** Pin count (for ->fd). */
	size_t pincount;
	/** An open descriptor to this file, or -1. */
	int fd;
	/** Whether this file has a pending ioq request. */
	bool ioqueued;
	/** An open directory for this file, if any. */
	struct bfs_dir *dir;

	/** This file's type, if known. */
	enum bfs_type type;
	/** The device number, for cycle detection. */
	dev_t dev;
	/** The inode number, for cycle detection. */
	ino_t ino;

	/** Cached bfs_stat() info. */
	struct bftw_stat stat_bufs;

	/** The offset of this file in the full path. */
	size_t nameoff;
	/** The length of the file's name. */
	size_t namelen;
	/** The file's name. */
	char name[];
};

/**
 * A linked list of bftw_file's.
 */
struct bftw_list {
	struct bftw_file *head;
	struct bftw_file **tail;
};

/**
 * bftw_queue flags.
 */
enum bftw_qflags {
	/** Track the sync/async service balance. */
	BFTW_QBALANCE = 1 << 0,
	/** Buffer files before adding them to the queue. */
	BFTW_QBUFFER  = 1 << 1,
	/** Use LIFO (stack/DFS) ordering. */
	BFTW_QLIFO    = 1 << 2,
	/** Maintain a strict order. */
	BFTW_QORDER   = 1 << 3,
};

/**
 * A queue of bftw_file's that may be serviced asynchronously.
 *
 * A bftw_queue comprises three linked lists each tracking different stages.
 * When BFTW_QBUFFER is set, files are initially pushed to the buffer:
 *
 *              ╔═══╗                 ╔═══╦═══╗
 *     buffer:  ║ 𝘩 ║                 ║ 𝘩 ║ 𝘪 ║
 *              ╠═══╬═══╦═══╗         ╠═══╬═══╬═══╗
 *     waiting: ║ e ║ f ║ g ║      →  ║ e ║ f ║ g ║
 *              ╠═══╬═══╬═══╬═══╗     ╠═══╬═══╬═══╬═══╗
 *     ready:   ║ 𝕒 ║ 𝕓 ║ 𝕔 ║ 𝕕 ║     ║ 𝕒 ║ 𝕓 ║ 𝕔 ║ 𝕕 ║
 *              ╚═══╩═══╩═══╩═══╝     ╚═══╩═══╩═══╩═══╝
 *
 * When bftw_queue_flush() is called, the files in the buffer are appended to
 * the waiting list (or prepended, if BFTW_QLIFO is set):
 *
 *              ╔═╗
 *     buffer:  ║ ║
 *              ╠═╩═╦═══╦═══╦═══╦═══╗
 *     waiting: ║ e ║ f ║ g ║ h ║ i ║
 *              ╠═══╬═══╬═══╬═══╬═══╝
 *     ready:   ║ 𝕒 ║ 𝕓 ║ 𝕔 ║ 𝕕 ║
 *              ╚═══╩═══╩═══╩═══╝
 *
 * Using the buffer gives a more natural ordering for BFTW_QLIFO, and allows
 * files to be sorted before adding them to the waiting list.  If BFTW_QBUFFER
 * is not set, files are pushed directly to the waiting list instead.
 *
 * Files on the waiting list are waiting to be "serviced" asynchronously by the
 * ioq (for example, by an ioq_opendir() or ioq_stat() call).  While they are
 * being serviced, they are detached from the queue by bftw_queue_detach() and
 * are not tracked by the queue at all:
 *
 *              ╔═╗
 *     buffer:  ║ ║
 *              ╠═╩═╦═══╦═══╗       ⎛      ┌───┬───┐ ⎞
 *     waiting: ║ g ║ h ║ i ║       ⎜ ioq: │ 𝓮 │ 𝓯 │ ⎟
 *              ╠═══╬═══╬═══╬═══╗   ⎝      └───┴───┘ ⎠
 *     ready:   ║ 𝕒 ║ 𝕓 ║ 𝕔 ║ 𝕕 ║
 *              ╚═══╩═══╩═══╩═══╝
 *
 * When their async service is complete, files are reattached to the queue by
 * bftw_queue_attach(), this time on the ready list:
 *
 *              ╔═╗
 *     buffer:  ║ ║
 *              ╠═╩═╦═══╦═══╗           ⎛      ┌───┐ ⎞
 *     waiting: ║ g ║ h ║ i ║           ⎜ ioq: │ 𝓮 │ ⎟
 *              ╠═══╬═══╬═══╬═══╦═══╗   ⎝      └───┘ ⎠
 *     ready:   ║ 𝕒 ║ 𝕓 ║ 𝕔 ║ 𝕕 ║ 𝕗 ║
 *              ╚═══╩═══╩═══╩═══╩═══╝
 *
 * Files are added to the ready list in the order they are finished by the ioq.
 * bftw_queue_pop() pops a file from the ready list if possible.  Otherwise, it
 * pops from the waiting list, and the file must be serviced synchronously.
 *
 * However, if BFTW_QORDER is set, files must be popped in the exact order they
 * are added to the waiting list (to maintain sorted order).  In this case,
 * files are added to the waiting and ready lists at the same time.  The
 * file->ioqueued flag is set while it is in-service, so that bftw() can wait
 * for it to be truly ready before using it.
 *
 *              ╔═╗
 *     buffer:  ║ ║
 *              ╠═╩═╦═══╦═══╗                           ⎛      ┌───┐ ⎞
 *     waiting: ║ g ║ h ║ i ║                           ⎜ ioq: │ 𝓮 │ ⎟
 *              ╠═══╬═══╬═══╬═══╦═══╦═══╦═══╦═══╦═══╗   ⎝      └───┘ ⎠
 *     ready:   ║ 𝕒 ║ 𝕓 ║ 𝕔 ║ 𝕕 ║ 𝓮 ║ 𝕗 ║ g ║ h ║ i ║
 *              ╚═══╩═══╩═══╩═══╩═══╩═══╩═══╩═══╩═══╝
 *
 * If BFTW_QBALANCE is set, queue->imbalance tracks the delta between async
 * service (negative) and synchronous service (positive).  The queue is
 * considered "balanced" when this number is non-negative.  Only a balanced
 * queue will perform any async service, ensuring work is fairly distributed
 * between the main thread and the ioq.
 *
 * BFTW_QBALANCE is only set for single-threaded ioqs.  When an ioq has multiple
 * threads, it is faster to wait for the ioq to complete an operation than it is
 * to perform it on the main thread.
 */
struct bftw_queue {
	/** Queue flags. */
	enum bftw_qflags flags;
	/** A buffer of files to be enqueued together. */
	struct bftw_list buffer;
	/** A list of files which are waiting to be serviced. */
	struct bftw_list waiting;
	/** A list of already-serviced files. */
	struct bftw_list ready;
	/** The current size of the queue. */
	size_t size;
	/** The number of files currently in-service. */
	size_t ioqueued;
	/** Tracks the imbalance between synchronous and async service. */
	unsigned long imbalance;
};

/** Initialize a queue. */
static void bftw_queue_init(struct bftw_queue *queue, enum bftw_qflags flags) {
	queue->flags = flags;
	SLIST_INIT(&queue->buffer);
	SLIST_INIT(&queue->waiting);
	SLIST_INIT(&queue->ready);
	queue->size = 0;
	queue->ioqueued = 0;
	queue->imbalance = 0;
}

/** Add a file to the queue. */
static void bftw_queue_push(struct bftw_queue *queue, struct bftw_file *file) {
	if (queue->flags & BFTW_QBUFFER) {
		SLIST_APPEND(&queue->buffer, file);
	} else if (queue->flags & BFTW_QLIFO) {
		SLIST_PREPEND(&queue->waiting, file);
		if (queue->flags & BFTW_QORDER) {
			SLIST_PREPEND(&queue->ready, file, ready);
		}
	} else {
		SLIST_APPEND(&queue->waiting, file);
		if (queue->flags & BFTW_QORDER) {
			SLIST_APPEND(&queue->ready, file, ready);
		}
	}

	++queue->size;
}

/** Add any buffered files to the queue. */
static void bftw_queue_flush(struct bftw_queue *queue) {
	if (!(queue->flags & BFTW_QBUFFER)) {
		return;
	}

	if (queue->flags & BFTW_QORDER) {
		// When sorting, add files to the ready list at the same time
		// (and in the same order) as they are added to the waiting list
		struct bftw_file **cursor = (queue->flags & BFTW_QLIFO)
			? &queue->ready.head
			: queue->ready.tail;
		for_slist (struct bftw_file, file, &queue->buffer) {
			cursor = SLIST_INSERT(&queue->ready, cursor, file, ready);
		}
	}

	if (queue->flags & BFTW_QLIFO) {
		SLIST_EXTEND(&queue->buffer, &queue->waiting);
	}

	SLIST_EXTEND(&queue->waiting, &queue->buffer);
}

/** Check if the queue is properly balanced for async work. */
static bool bftw_queue_balanced(const struct bftw_queue *queue) {
	if (queue->flags & BFTW_QBALANCE) {
		return (long)queue->imbalance >= 0;
	} else {
		return true;
	}
}

/** Update the queue balance for (a)sync service. */
static void bftw_queue_rebalance(struct bftw_queue *queue, bool async) {
	if (async) {
		--queue->imbalance;
	} else {
		++queue->imbalance;
	}
}

/** Detatch the next waiting file. */
static void bftw_queue_detach(struct bftw_queue *queue, struct bftw_file *file, bool async) {
	bfs_assert(!file->ioqueued);

	if (file == SLIST_HEAD(&queue->buffer)) {
		// To maintain order, we can't detach any files until they're
		// added to the waiting/ready lists
		bfs_assert(!(queue->flags & BFTW_QORDER));
		SLIST_POP(&queue->buffer);
	} else if (file == SLIST_HEAD(&queue->waiting)) {
		SLIST_POP(&queue->waiting);
	} else {
		bfs_bug("Detached file was not buffered or waiting");
	}

	if (async) {
		file->ioqueued = true;
		++queue->ioqueued;
		bftw_queue_rebalance(queue, true);
	}
}

/** Reattach a serviced file to the queue. */
static void bftw_queue_attach(struct bftw_queue *queue, struct bftw_file *file, bool async) {
	if (async) {
		bfs_assert(file->ioqueued);
		file->ioqueued = false;
		--queue->ioqueued;
	} else {
		bfs_assert(!file->ioqueued);
	}

	if (!(queue->flags & BFTW_QORDER)) {
		SLIST_APPEND(&queue->ready, file, ready);
	}
}

/** Make a file ready immediately. */
static void bftw_queue_skip(struct bftw_queue *queue, struct bftw_file *file) {
	bftw_queue_detach(queue, file, false);
	bftw_queue_attach(queue, file, false);
}

/** Get the next waiting file. */
static struct bftw_file *bftw_queue_waiting(const struct bftw_queue *queue) {
	if (!(queue->flags & BFTW_QBUFFER)) {
		return SLIST_HEAD(&queue->waiting);
	}

	if (queue->flags & BFTW_QORDER) {
		// Don't detach files until they're on the waiting/ready lists
		return SLIST_HEAD(&queue->waiting);
	}

	const struct bftw_list *prefix = &queue->waiting;
	const struct bftw_list *suffix = &queue->buffer;
	if (queue->flags & BFTW_QLIFO) {
		prefix = &queue->buffer;
		suffix = &queue->waiting;
	}

	struct bftw_file *file = SLIST_HEAD(prefix);
	if (!file) {
		file = SLIST_HEAD(suffix);
	}
	return file;
}

/** Get the next ready file. */
static struct bftw_file *bftw_queue_ready(const struct bftw_queue *queue) {
	return SLIST_HEAD(&queue->ready);
}

/** Pop a file from the queue. */
static struct bftw_file *bftw_queue_pop(struct bftw_queue *queue) {
	// Don't pop until we've had a chance to sort the buffer
	bfs_assert(SLIST_EMPTY(&queue->buffer));

	struct bftw_file *file = SLIST_POP(&queue->ready, ready);

	if (!file || file == SLIST_HEAD(&queue->waiting)) {
		// If no files are ready, try the waiting list.  Or, if
		// BFTW_QORDER is set, we may need to pop from both lists.
		file = SLIST_POP(&queue->waiting);
	}

	if (file) {
		--queue->size;
	}

	return file;
}

/**
 * A cache of open directories.
 */
struct bftw_cache {
	/** The head of the LRU list. */
	struct bftw_file *head;
	/** The tail of the LRU list. */
	struct bftw_file *tail;
	/** The insertion target for the LRU list. */
	struct bftw_file *target;
	/** The remaining capacity of the LRU list. */
	size_t capacity;

	/** bftw_file arena. */
	struct varena files;

	/** bfs_dir arena. */
	struct arena dirs;
	/** Remaining bfs_dir capacity. */
	int dir_limit;

	/** bfs_stat arena. */
	struct arena stat_bufs;
};

/** Initialize a cache. */
static void bftw_cache_init(struct bftw_cache *cache, size_t capacity) {
	LIST_INIT(cache);
	cache->target = NULL;
	cache->capacity = capacity;

	VARENA_INIT(&cache->files, struct bftw_file, name);

	bfs_dir_arena(&cache->dirs);

	if (cache->capacity > 1024) {
		cache->dir_limit = 1024;
	} else {
		cache->dir_limit = capacity - 1;
	}

	ARENA_INIT(&cache->stat_bufs, struct bfs_stat);
}

/** Allocate a directory. */
static struct bfs_dir *bftw_allocdir(struct bftw_cache *cache, bool force) {
	if (!force && cache->dir_limit <= 0) {
		errno = ENOMEM;
		return NULL;
	}

	struct bfs_dir *dir = arena_alloc(&cache->dirs);
	if (dir) {
		--cache->dir_limit;
	}
	return dir;
}

/** Free a directory. */
static void bftw_freedir(struct bftw_cache *cache, struct bfs_dir *dir) {
	++cache->dir_limit;
	arena_free(&cache->dirs, dir);
}

/** Remove a bftw_file from the LRU list. */
static void bftw_lru_remove(struct bftw_cache *cache, struct bftw_file *file) {
	if (cache->target == file) {
		cache->target = file->lru.prev;
	}

	LIST_REMOVE(cache, file, lru);
}

/** Remove a bftw_file from the cache. */
static void bftw_cache_remove(struct bftw_cache *cache, struct bftw_file *file) {
	bftw_lru_remove(cache, file);
	++cache->capacity;
}

/** Close a bftw_file. */
static void bftw_file_close(struct bftw_cache *cache, struct bftw_file *file) {
	bfs_assert(file->fd >= 0);
	bfs_assert(file->pincount == 0);

	if (file->dir) {
		bfs_closedir(file->dir);
		bftw_freedir(cache, file->dir);
		file->dir = NULL;
	} else {
		xclose(file->fd);
	}

	file->fd = -1;
	bftw_cache_remove(cache, file);
}

/** Pop the least recently used directory from the cache. */
static int bftw_cache_pop(struct bftw_cache *cache) {
	struct bftw_file *file = cache->tail;
	if (!file) {
		return -1;
	}

	bftw_file_close(cache, file);
	return 0;
}

/** Add a bftw_file to the LRU list. */
static void bftw_lru_add(struct bftw_cache *cache, struct bftw_file *file) {
	bfs_assert(file->fd >= 0);

	LIST_INSERT(cache, cache->target, file, lru);

	// Prefer to keep the root paths open by keeping them at the head of the list
	if (file->depth == 0) {
		cache->target = file;
	}
}

/** Add a bftw_file to the cache. */
static int bftw_cache_add(struct bftw_cache *cache, struct bftw_file *file) {
	bfs_assert(file->fd >= 0);

	if (cache->capacity == 0 && bftw_cache_pop(cache) != 0) {
		bftw_file_close(cache, file);
		errno = EMFILE;
		return -1;
	}

	bfs_assert(cache->capacity > 0);
	--cache->capacity;

	bftw_lru_add(cache, file);
	return 0;
}

/** Pin a cache entry so it won't be closed. */
static void bftw_cache_pin(struct bftw_cache *cache, struct bftw_file *file) {
	bfs_assert(file->fd >= 0);

	if (file->pincount++ == 0) {
		bftw_lru_remove(cache, file);
	}
}

/** Unpin a cache entry. */
static void bftw_cache_unpin(struct bftw_cache *cache, struct bftw_file *file) {
	bfs_assert(file->fd >= 0);
	bfs_assert(file->pincount > 0);

	if (--file->pincount == 0) {
		bftw_lru_add(cache, file);
	}
}

/** Compute the name offset of a child path. */
static size_t bftw_child_nameoff(const struct bftw_file *parent) {
	size_t ret = parent->nameoff + parent->namelen;
	if (parent->name[parent->namelen - 1] != '/') {
		++ret;
	}
	return ret;
}

/** Destroy a cache. */
static void bftw_cache_destroy(struct bftw_cache *cache) {
	bfs_assert(LIST_EMPTY(cache));
	bfs_assert(!cache->target);

	arena_destroy(&cache->stat_bufs);
	arena_destroy(&cache->dirs);
	varena_destroy(&cache->files);
}

/** Create a new bftw_file. */
static struct bftw_file *bftw_file_new(struct bftw_cache *cache, struct bftw_file *parent, const char *name) {
	size_t namelen = strlen(name);
	struct bftw_file *file = varena_alloc(&cache->files, namelen + 1);
	if (!file) {
		return NULL;
	}

	file->parent = parent;

	if (parent) {
		file->root = parent->root;
		file->depth = parent->depth + 1;
		file->nameoff = bftw_child_nameoff(parent);
		++parent->refcount;
	} else {
		file->root = file;
		file->depth = 0;
		file->nameoff = 0;
	}

	SLIST_ITEM_INIT(file);
	SLIST_ITEM_INIT(file, ready);
	LIST_ITEM_INIT(file, lru);

	file->refcount = 1;
	file->pincount = 0;
	file->fd = -1;
	file->ioqueued = false;
	file->dir = NULL;

	file->type = BFS_UNKNOWN;
	file->dev = -1;
	file->ino = -1;

	bftw_stat_init(&file->stat_bufs, NULL, NULL);

	file->namelen = namelen;
	memcpy(file->name, name, namelen + 1);

	return file;
}

/** Associate an open directory with a bftw_file. */
static void bftw_file_set_dir(struct bftw_cache *cache, struct bftw_file *file, struct bfs_dir *dir) {
	bfs_assert(!file->dir);
	file->dir = dir;

	if (file->fd >= 0) {
		bfs_assert(file->fd == bfs_dirfd(dir));
	} else {
		file->fd = bfs_dirfd(dir);
		bftw_cache_add(cache, file);
	}
}

/** Free a file's cached stat() buffers. */
static void bftw_stat_recycle(struct bftw_cache *cache, struct bftw_file *file) {
	struct bftw_stat *bufs = &file->stat_bufs;

	struct bfs_stat *stat_buf = (struct bfs_stat *)bufs->stat_buf;
	struct bfs_stat *lstat_buf = (struct bfs_stat *)bufs->lstat_buf;
	if (stat_buf) {
		arena_free(&cache->stat_bufs, stat_buf);
	} else if (lstat_buf) {
		arena_free(&cache->stat_bufs, lstat_buf);
	}

	bftw_stat_init(bufs, NULL, NULL);
}

/** Free a bftw_file. */
static void bftw_file_free(struct bftw_cache *cache, struct bftw_file *file) {
	bfs_assert(file->refcount == 0);

	if (file->fd >= 0) {
		bftw_file_close(cache, file);
	}

	bftw_stat_recycle(cache, file);

	varena_free(&cache->files, file, file->namelen + 1);
}

/**
 * Holds the current state of the bftw() traversal.
 */
struct bftw_state {
	/** The path(s) to start from. */
	const char **paths;
	/** The number of starting paths. */
	size_t npaths;
	/** bftw() callback. */
	bftw_callback *callback;
	/** bftw() callback data. */
	void *ptr;
	/** bftw() flags. */
	enum bftw_flags flags;
	/** Search strategy. */
	enum bftw_strategy strategy;
	/** The mount table. */
	const struct bfs_mtab *mtab;
	/** bfs_opendir() flags. */
	enum bfs_dir_flags dir_flags;

	/** The appropriate errno value, if any. */
	int error;

	/** The cache of open directories. */
	struct bftw_cache cache;

	/** The async I/O queue. */
	struct ioq *ioq;
	/** The number of I/O threads. */
	size_t nthreads;

	/** The queue of unpinned directories to unwrap. */
	struct bftw_list to_close;
	/** The queue of files to visit. */
	struct bftw_queue fileq;
	/** The queue of directories to open/read. */
	struct bftw_queue dirq;

	/** The current path. */
	dchar *path;
	/** The current file. */
	struct bftw_file *file;
	/** The previous file. */
	struct bftw_file *previous;

	/** The currently open directory. */
	struct bfs_dir *dir;
	/** The current directory entry. */
	struct bfs_dirent *de;
	/** Storage for the directory entry. */
	struct bfs_dirent de_storage;
	/** Any error encountered while reading the directory. */
	int direrror;

	/** Extra data about the current file. */
	struct BFTW ftwbuf;
	/** stat() buffer storage. */
	struct bfs_stat stat_buf;
	/** lstat() buffer storage. */
	struct bfs_stat lstat_buf;
};

/** Check if we have to buffer files before visiting them. */
static bool bftw_must_buffer(const struct bftw_state *state) {
	if (state->flags & BFTW_SORT) {
		// Have to buffer the files to sort them
		return true;
	}

	if (state->strategy == BFTW_DFS && state->nthreads == 0) {
		// Without buffering, we would get a not-quite-depth-first
		// ordering:
		//
		//     a
		//     b
		//     a/c
		//     a/c/d
		//     b/e
		//     b/e/f
		//
		// This is okay for iterative deepening, since the caller only
		// sees files at the target depth.  We also deem it okay for
		// parallel searches, since the order is unpredictable anyway.
		return true;
	}

	if ((state->flags & BFTW_STAT) && state->nthreads > 1) {
		// We will be buffering every file anyway for ioq_stat()
		return true;
	}

	return false;
}

/** Initialize the bftw() state. */
static int bftw_state_init(struct bftw_state *state, const struct bftw_args *args) {
	state->paths = args->paths;
	state->npaths = args->npaths;
	state->callback = args->callback;
	state->ptr = args->ptr;
	state->flags = args->flags;
	state->strategy = args->strategy;
	state->mtab = args->mtab;
	state->dir_flags = 0;
	state->error = 0;

	if (args->nopenfd < 2) {
		errno = EMFILE;
		return -1;
	}

	size_t nopenfd = args->nopenfd;
	size_t qdepth = 4096;
	size_t nthreads = args->nthreads;

#if BFS_USE_LIBURING
	// io_uring uses one fd per ring, ioq uses one ring per thread
	if (nthreads >= nopenfd - 1) {
		nthreads = nopenfd - 2;
	}
	nopenfd -= nthreads;
#endif

	bftw_cache_init(&state->cache, nopenfd);

	if (nthreads > 0) {
		state->ioq = ioq_create(qdepth, nthreads);
		if (!state->ioq) {
			return -1;
		}
	} else {
		state->ioq = NULL;
	}
	state->nthreads = nthreads;

	if (bftw_must_buffer(state)) {
		state->flags |= BFTW_BUFFER;
	}

	if (state->flags & BFTW_WHITEOUTS) {
		state->dir_flags |= BFS_DIR_WHITEOUTS;
	}

	SLIST_INIT(&state->to_close);

	enum bftw_qflags qflags = 0;
	if (state->strategy != BFTW_BFS) {
		qflags |= BFTW_QBUFFER | BFTW_QLIFO;
	}
	if (state->flags & BFTW_BUFFER) {
		qflags |= BFTW_QBUFFER;
	}
	if (state->flags & BFTW_SORT) {
		qflags |= BFTW_QORDER;
	} else if (nthreads == 1) {
		qflags |= BFTW_QBALANCE;
	}
	bftw_queue_init(&state->fileq, qflags);

	if (state->strategy == BFTW_BFS || (state->flags & BFTW_BUFFER)) {
		// In breadth-first mode, or if we're already buffering files,
		// directories can be queued in FIFO order
		qflags &= ~(BFTW_QBUFFER | BFTW_QLIFO);
	}
	bftw_queue_init(&state->dirq, qflags);

	state->path = NULL;
	state->file = NULL;
	state->previous = NULL;

	state->dir = NULL;
	state->de = NULL;
	state->direrror = 0;

	return 0;
}

/** Queue a directory for unwrapping. */
static void bftw_delayed_unwrap(struct bftw_state *state, struct bftw_file *file) {
	bfs_assert(file->dir);

	if (!SLIST_ATTACHED(&state->to_close, file, ready)) {
		SLIST_APPEND(&state->to_close, file, ready);
	}
}

/** Unpin a file's parent. */
static void bftw_unpin_parent(struct bftw_state *state, struct bftw_file *file, bool unwrap) {
	struct bftw_file *parent = file->parent;
	if (!parent) {
		return;
	}

	bftw_cache_unpin(&state->cache, parent);

	if (unwrap && parent->dir && parent->pincount == 0) {
		bftw_delayed_unwrap(state, parent);
	}
}

/** Pop a response from the I/O queue. */
static int bftw_ioq_pop(struct bftw_state *state, bool block) {
	struct bftw_cache *cache = &state->cache;
	struct ioq *ioq = state->ioq;
	if (!ioq) {
		return -1;
	}

	struct ioq_ent *ent = ioq_pop(ioq, block);
	if (!ent) {
		return -1;
	}

	struct bftw_file *file = ent->ptr;
	if (file) {
		bftw_unpin_parent(state, file, true);
	}

	enum ioq_op op = ent->op;
	switch (op) {
	case IOQ_CLOSE:
		++cache->capacity;
		break;

	case IOQ_CLOSEDIR:
		++cache->capacity;
		bftw_freedir(cache, ent->closedir.dir);
		break;

	case IOQ_OPENDIR:
		++cache->capacity;

		if (ent->result >= 0) {
			bftw_file_set_dir(cache, file, ent->opendir.dir);
		} else {
			bftw_freedir(cache, ent->opendir.dir);
		}

		bftw_queue_attach(&state->dirq, file, true);
		break;

	case IOQ_STAT:
		if (ent->result >= 0) {
			bftw_stat_cache(&file->stat_bufs, ent->stat.flags, ent->stat.buf, 0);
		} else {
			arena_free(&cache->stat_bufs, ent->stat.buf);
			bftw_stat_cache(&file->stat_bufs, ent->stat.flags, NULL, -ent->result);
		}

		bftw_queue_attach(&state->fileq, file, true);
		break;
	}

	ioq_free(ioq, ent);
	return op;
}

/** Try to reserve space in the I/O queue. */
static int bftw_ioq_reserve(struct bftw_state *state) {
	struct ioq *ioq = state->ioq;
	if (!ioq) {
		return -1;
	}

	if (ioq_capacity(ioq) > 0) {
		return 0;
	}

	// With more than one background thread, it's faster to wait on
	// background I/O than it is to do it on the main thread
	bool block = state->nthreads > 1;
	if (bftw_ioq_pop(state, block) < 0) {
		return -1;
	}

	return 0;
}

/** Try to reserve space in the cache. */
static int bftw_cache_reserve(struct bftw_state *state) {
	struct bftw_cache *cache = &state->cache;
	if (cache->capacity > 0) {
		return 0;
	}

	while (bftw_ioq_pop(state, true) >= 0) {
		if (cache->capacity > 0) {
			return 0;
		}
	}

	if (bftw_cache_pop(cache) != 0) {
		errno = EMFILE;
		return -1;
	}

	bfs_assert(cache->capacity > 0);
	return 0;
}

/** Open a bftw_file relative to another one. */
static int bftw_file_openat(struct bftw_state *state, struct bftw_file *file, struct bftw_file *base, const char *at_path) {
	bfs_assert(file->fd < 0);

	struct bftw_cache *cache = &state->cache;

	int at_fd = AT_FDCWD;
	if (base) {
		bftw_cache_pin(cache, base);
		at_fd = base->fd;
	}

	int fd = -1;
	if (bftw_cache_reserve(state) != 0) {
		goto unpin;
	}

	int flags = O_RDONLY | O_CLOEXEC | O_DIRECTORY;
	fd = openat(at_fd, at_path, flags);

	if (fd < 0 && errno == EMFILE) {
		if (bftw_cache_pop(cache) == 0) {
			fd = openat(at_fd, at_path, flags);
		}
		cache->capacity = 1;
	}

unpin:
	if (base) {
		bftw_cache_unpin(cache, base);
	}

	if (fd >= 0) {
		file->fd = fd;
		bftw_cache_add(cache, file);
	}

	return fd;
}

/** Open a bftw_file. */
static int bftw_file_open(struct bftw_state *state, struct bftw_file *file, const char *path) {
	// Find the nearest open ancestor
	struct bftw_file *base = file;
	do {
		base = base->parent;
	} while (base && base->fd < 0);

	const char *at_path = path;
	if (base) {
		at_path += bftw_child_nameoff(base);
	}

	int fd = bftw_file_openat(state, file, base, at_path);
	if (fd >= 0 || !errno_is_like(ENAMETOOLONG)) {
		return fd;
	}

	// Handle ENAMETOOLONG by manually traversing the path component-by-component
	struct bftw_list parents;
	SLIST_INIT(&parents);

	struct bftw_file *cur;
	for (cur = file; cur != base; cur = cur->parent) {
		SLIST_PREPEND(&parents, cur);
	}

	while ((cur = SLIST_POP(&parents))) {
		if (!cur->parent || cur->parent->fd >= 0) {
			bftw_file_openat(state, cur, cur->parent, cur->name);
		}
	}

	return file->fd;
}

/** Close a directory, asynchronously if possible. */
static int bftw_ioq_closedir(struct bftw_state *state, struct bfs_dir *dir) {
	if (bftw_ioq_reserve(state) == 0) {
		if (ioq_closedir(state->ioq, dir, NULL) == 0) {
			return 0;
		}
	}

	struct bftw_cache *cache = &state->cache;
	int ret = bfs_closedir(dir);
	bftw_freedir(cache, dir);
	++cache->capacity;
	return ret;
}

/** Close a file descriptor, asynchronously if possible. */
static int bftw_ioq_close(struct bftw_state *state, int fd) {
	if (bftw_ioq_reserve(state) == 0) {
		if (ioq_close(state->ioq, fd, NULL) == 0) {
			return 0;
		}
	}

	struct bftw_cache *cache = &state->cache;
	int ret = xclose(fd);
	++cache->capacity;
	return ret;
}

/** Close a file, asynchronously if possible. */
static int bftw_close(struct bftw_state *state, struct bftw_file *file) {
	bfs_assert(file->fd >= 0);
	bfs_assert(file->pincount == 0);

	struct bfs_dir *dir = file->dir;
	int fd = file->fd;

	bftw_lru_remove(&state->cache, file);
	file->dir = NULL;
	file->fd = -1;

	if (dir) {
		return bftw_ioq_closedir(state, dir);
	} else {
		return bftw_ioq_close(state, fd);
	}
}

/** Free an open directory. */
static int bftw_unwrapdir(struct bftw_state *state, struct bftw_file *file) {
	struct bfs_dir *dir = file->dir;
	if (!dir) {
		return 0;
	}

	struct bftw_cache *cache = &state->cache;

	// Try to keep an open fd if any children exist
	bool reffed = file->refcount > 1;
	// Keep the fd the same if it's pinned
	bool pinned = file->pincount > 0;

#if BFS_USE_UNWRAPDIR
	if (reffed || pinned) {
		bfs_unwrapdir(dir);
		bftw_freedir(cache, dir);
		file->dir = NULL;
		return 0;
	}
#else
	if (pinned) {
		return -1;
	}
#endif

	if (!reffed) {
		return bftw_close(state, file);
	}

	// Make room for dup()
	bftw_cache_pin(cache, file);
	int ret = bftw_cache_reserve(state);
	bftw_cache_unpin(cache, file);
	if (ret != 0) {
		return ret;
	}

	int fd = dup_cloexec(file->fd);
	if (fd < 0) {
		return -1;
	}
	--cache->capacity;

	file->dir = NULL;
	file->fd = fd;
	return bftw_ioq_closedir(state, dir);
}

/** Try to pin a file's parent. */
static int bftw_pin_parent(struct bftw_state *state, struct bftw_file *file) {
	struct bftw_file *parent = file->parent;
	if (!parent) {
		return AT_FDCWD;
	}

	int fd = parent->fd;
	if (fd < 0) {
		bfs_static_assert(AT_FDCWD != -1);
		return -1;
	}

	bftw_cache_pin(&state->cache, parent);
	return fd;
}

/** Open a directory asynchronously. */
static int bftw_ioq_opendir(struct bftw_state *state, struct bftw_file *file) {
	struct bftw_cache *cache = &state->cache;

	if (bftw_ioq_reserve(state) != 0) {
		goto fail;
	}

	int dfd = bftw_pin_parent(state, file);
	if (dfd < 0 && dfd != AT_FDCWD) {
		goto fail;
	}

	if (bftw_cache_reserve(state) != 0) {
		goto unpin;
	}

	struct bfs_dir *dir = bftw_allocdir(cache, false);
	if (!dir) {
		goto unpin;
	}

	if (ioq_opendir(state->ioq, dir, dfd, file->name, state->dir_flags, file) != 0) {
		goto free;
	}

	--cache->capacity;
	return 0;

free:
	bftw_freedir(cache, dir);
unpin:
	bftw_unpin_parent(state, file, false);
fail:
	return -1;
}

/** Open a batch of directories asynchronously. */
static void bftw_ioq_opendirs(struct bftw_state *state) {
	while (bftw_queue_balanced(&state->dirq)) {
		struct bftw_file *dir = bftw_queue_waiting(&state->dirq);
		if (!dir) {
			break;
		}

		if (bftw_ioq_opendir(state, dir) == 0) {
			bftw_queue_detach(&state->dirq, dir, true);
		} else {
			break;
		}
	}
}

/** Push a directory onto the queue. */
static void bftw_push_dir(struct bftw_state *state, struct bftw_file *file) {
	bfs_assert(file->type == BFS_DIR);
	bftw_queue_push(&state->dirq, file);
	bftw_ioq_opendirs(state);
}

/** Pop a file from a queue, then activate it. */
static bool bftw_pop(struct bftw_state *state, struct bftw_queue *queue) {
	if (queue->size == 0) {
		return false;
	}

	while (!bftw_queue_ready(queue) && queue->ioqueued > 0) {
		bool block = true;
		if (bftw_queue_waiting(queue) && state->nthreads == 1) {
			// With only one background thread, balance the work
			// between it and the main thread
			block = false;
		}

		if (bftw_ioq_pop(state, block) < 0) {
			break;
		}
	}

	struct bftw_file *file = bftw_queue_pop(queue);
	if (!file) {
		return false;
	}

	while (file->ioqueued) {
		bftw_ioq_pop(state, true);
	}

	state->file = file;
	return true;
}

/** Pop a directory to read from the queue. */
static bool bftw_pop_dir(struct bftw_state *state) {
	bfs_assert(!state->file);

	if (state->flags & BFTW_SORT) {
		// Keep strict breadth-first order when sorting
		if (state->strategy == BFTW_BFS && bftw_queue_ready(&state->fileq)) {
			return false;
		}
	} else if (!bftw_queue_ready(&state->dirq)) {
		// Don't block if we have files ready to visit
		if (bftw_queue_ready(&state->fileq)) {
			return false;
		}
	}

	return bftw_pop(state, &state->dirq);
}

/** Figure out bfs_stat() flags. */
static enum bfs_stat_flags bftw_stat_flags(const struct bftw_state *state, size_t depth) {
	enum bftw_flags mask = BFTW_FOLLOW_ALL;
	if (depth == 0) {
		mask |= BFTW_FOLLOW_ROOTS;
	}

	if (state->flags & mask) {
		return BFS_STAT_TRYFOLLOW;
	} else {
		return BFS_STAT_NOFOLLOW;
	}
}

/** Check if a stat() call is necessary. */
static bool bftw_must_stat(const struct bftw_state *state, size_t depth, enum bfs_type type, const char *name) {
	if (state->flags & BFTW_STAT) {
		return true;
	}

	switch (type) {
	case BFS_UNKNOWN:
		return true;

	case BFS_DIR:
		return state->flags & (BFTW_DETECT_CYCLES | BFTW_SKIP_MOUNTS | BFTW_PRUNE_MOUNTS);

	case BFS_LNK:
		if (!(bftw_stat_flags(state, depth) & BFS_STAT_NOFOLLOW)) {
			return true;
		}
		fallthru;

	default:
#if __linux__
		if (state->mtab && bfs_might_be_mount(state->mtab, name)) {
			return true;
		}
#endif
		return false;
	}
}

/** stat() a file asynchronously. */
static int bftw_ioq_stat(struct bftw_state *state, struct bftw_file *file) {
	if (bftw_ioq_reserve(state) != 0) {
		goto fail;
	}

	int dfd = bftw_pin_parent(state, file);
	if (dfd < 0 && dfd != AT_FDCWD) {
		goto fail;
	}

	struct bftw_cache *cache = &state->cache;
	struct bfs_stat *buf = arena_alloc(&cache->stat_bufs);
	if (!buf) {
		goto unpin;
	}

	enum bfs_stat_flags flags = bftw_stat_flags(state, file->depth);
	if (ioq_stat(state->ioq, dfd, file->name, flags, buf, file) != 0) {
		goto free;
	}

	return 0;

free:
	arena_free(&cache->stat_bufs, buf);
unpin:
	bftw_unpin_parent(state, file, false);
fail:
	return -1;
}

/** Check if we should stat() a file asynchronously. */
static bool bftw_should_ioq_stat(struct bftw_state *state, struct bftw_file *file) {
	// To avoid surprising users too much, process the roots in order
	if (file->depth == 0) {
		return false;
	}

#ifdef S_IFWHT
	// ioq_stat() does not do whiteout emulation like bftw_stat_impl()
	if (file->type == BFS_WHT) {
		return false;
	}
#endif

	return bftw_must_stat(state, file->depth, file->type, file->name);
}

/** Call stat() on files that need it. */
static void bftw_stat_files(struct bftw_state *state) {
	while (true) {
		struct bftw_file *file = bftw_queue_waiting(&state->fileq);
		if (!file) {
			break;
		}

		if (!bftw_should_ioq_stat(state, file)) {
			bftw_queue_skip(&state->fileq, file);
			continue;
		}

		if (!bftw_queue_balanced(&state->fileq)) {
			break;
		}

		if (bftw_ioq_stat(state, file) == 0) {
			bftw_queue_detach(&state->fileq, file, true);
		} else {
			break;
		}
	}
}

/** Push a file onto the queue. */
static void bftw_push_file(struct bftw_state *state, struct bftw_file *file) {
	bftw_queue_push(&state->fileq, file);
	bftw_stat_files(state);
}

/** Pop a file to visit from the queue. */
static bool bftw_pop_file(struct bftw_state *state) {
	bfs_assert(!state->file);
	return bftw_pop(state, &state->fileq);
}

/** Build the path to the current file. */
static int bftw_build_path(struct bftw_state *state, const char *name) {
	const struct bftw_file *file = state->file;

	size_t pathlen = file ? file->nameoff + file->namelen : 0;
	if (dstresize(&state->path, pathlen) != 0) {
		state->error = errno;
		return -1;
	}

	// Try to find a common ancestor with the existing path
	const struct bftw_file *ancestor = state->previous;
	while (ancestor && ancestor->depth > file->depth) {
		ancestor = ancestor->parent;
	}

	// Build the path backwards
	while (file && file != ancestor) {
		if (file->nameoff > 0) {
			state->path[file->nameoff - 1] = '/';
		}
		memcpy(state->path + file->nameoff, file->name, file->namelen);

		if (ancestor && ancestor->depth == file->depth) {
			ancestor = ancestor->parent;
		}
		file = file->parent;
	}

	state->previous = state->file;

	if (name) {
		if (pathlen > 0 && state->path[pathlen - 1] != '/') {
			if (dstrapp(&state->path, '/') != 0) {
				state->error = errno;
				return -1;
			}
		}
		if (dstrcat(&state->path, name) != 0) {
			state->error = errno;
			return -1;
		}
	}

	return 0;
}

/** Open a bftw_file as a directory. */
static struct bfs_dir *bftw_file_opendir(struct bftw_state *state, struct bftw_file *file, const char *path) {
	int fd = bftw_file_open(state, file, path);
	if (fd < 0) {
		return NULL;
	}

	struct bftw_cache *cache = &state->cache;
	struct bfs_dir *dir = bftw_allocdir(cache, true);
	if (!dir) {
		return NULL;
	}

	if (bfs_opendir(dir, fd, NULL, state->dir_flags) != 0) {
		bftw_freedir(cache, dir);
		return NULL;
	}

	bftw_file_set_dir(cache, file, dir);
	return dir;
}

/** Open the current directory. */
static int bftw_opendir(struct bftw_state *state) {
	bfs_assert(!state->dir);
	bfs_assert(!state->de);

	state->direrror = 0;

	struct bftw_file *file = state->file;
	state->dir = file->dir;
	if (state->dir) {
		goto pin;
	}

	if (bftw_build_path(state, NULL) != 0) {
		return -1;
	}

	bftw_queue_rebalance(&state->dirq, false);

	state->dir = bftw_file_opendir(state, file, state->path);
	if (!state->dir) {
		state->direrror = errno;
		return 0;
	}

pin:
	bftw_cache_pin(&state->cache, file);
	return 0;
}

/** Read an entry from the current directory. */
static int bftw_readdir(struct bftw_state *state) {
	if (!state->dir) {
		return -1;
	}

	int ret = bfs_readdir(state->dir, &state->de_storage);
	if (ret > 0) {
		state->de = &state->de_storage;
	} else if (ret == 0) {
		state->de = NULL;
	} else {
		state->de = NULL;
		state->direrror = errno;
	}

	return ret;
}

/** Open a file if necessary. */
static int bftw_ensure_open(struct bftw_state *state, struct bftw_file *file, const char *path) {
	int ret = file->fd;

	if (ret < 0) {
		char *copy = strndup(path, file->nameoff + file->namelen);
		if (!copy) {
			return -1;
		}

		ret = bftw_file_open(state, file, copy);
		free(copy);
	}

	return ret;
}

/** Initialize the buffers with data about the current path. */
static void bftw_init_ftwbuf(struct bftw_state *state, enum bftw_visit visit) {
	struct bftw_file *file = state->file;
	const struct bfs_dirent *de = state->de;

	struct BFTW *ftwbuf = &state->ftwbuf;
	ftwbuf->path = state->path;
	ftwbuf->root = file ? file->root->name : ftwbuf->path;
	ftwbuf->depth = 0;
	ftwbuf->visit = visit;
	ftwbuf->type = BFS_UNKNOWN;
	ftwbuf->error = state->direrror;
	ftwbuf->at_fd = AT_FDCWD;
	ftwbuf->at_path = ftwbuf->path;
	bftw_stat_init(&ftwbuf->stat_bufs, &state->stat_buf, &state->lstat_buf);

	struct bftw_file *parent = NULL;
	if (de) {
		parent = file;
		ftwbuf->depth = file->depth + 1;
		ftwbuf->type = de->type;
		ftwbuf->nameoff = bftw_child_nameoff(file);
	} else if (file) {
		parent = file->parent;
		ftwbuf->depth = file->depth;
		ftwbuf->type = file->type;
		ftwbuf->nameoff = file->nameoff;
		bftw_stat_fill(&ftwbuf->stat_bufs, &file->stat_bufs);
	}

	if (parent) {
		// Try to ensure the immediate parent is open, to avoid ENAMETOOLONG
		if (bftw_ensure_open(state, parent, state->path) >= 0) {
			ftwbuf->at_fd = parent->fd;
			ftwbuf->at_path += ftwbuf->nameoff;
		} else {
			ftwbuf->error = errno;
		}
	}

	if (ftwbuf->depth == 0) {
		// Compute the name offset for root paths like "foo/bar"
		ftwbuf->nameoff = xbaseoff(ftwbuf->path);
	}

	ftwbuf->stat_flags = bftw_stat_flags(state, ftwbuf->depth);

	if (ftwbuf->error != 0) {
		ftwbuf->type = BFS_ERROR;
		return;
	}

	const struct bfs_stat *statbuf = NULL;
	if (bftw_must_stat(state, ftwbuf->depth, ftwbuf->type, ftwbuf->path + ftwbuf->nameoff)) {
		statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
		if (statbuf) {
			ftwbuf->type = bfs_mode_to_type(statbuf->mode);
		} else {
			ftwbuf->type = BFS_ERROR;
			ftwbuf->error = errno;
			return;
		}
	}

	if (ftwbuf->type == BFS_DIR && (state->flags & BFTW_DETECT_CYCLES)) {
		for (const struct bftw_file *ancestor = parent; ancestor; ancestor = ancestor->parent) {
			if (ancestor->dev == statbuf->dev && ancestor->ino == statbuf->ino) {
				ftwbuf->type = BFS_ERROR;
				ftwbuf->error = ELOOP;
				return;
			}
		}
	}
}

/** Check if the current file is a mount point. */
static bool bftw_is_mount(struct bftw_state *state, const char *name) {
	const struct bftw_file *file = state->file;
	if (!file) {
		return false;
	}

	const struct bftw_file *parent = name ? file : file->parent;
	if (!parent) {
		return false;
	}

	const struct BFTW *ftwbuf = &state->ftwbuf;
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	return statbuf && statbuf->dev != parent->dev;
}

/** Check if bfs_stat() was called from the main thread. */
static bool bftw_stat_was_sync(const struct bftw_state *state, const struct bfs_stat *buf) {
	return buf == &state->stat_buf || buf == &state->lstat_buf;
}

/** Invoke the callback. */
static enum bftw_action bftw_call_back(struct bftw_state *state, const char *name, enum bftw_visit visit) {
	if (visit == BFTW_POST && !(state->flags & BFTW_POST_ORDER)) {
		return BFTW_PRUNE;
	}

	if (bftw_build_path(state, name) != 0) {
		return BFTW_STOP;
	}

	const struct BFTW *ftwbuf = &state->ftwbuf;
	bftw_init_ftwbuf(state, visit);

	// Never give the callback BFS_ERROR unless BFTW_RECOVER is specified
	if (ftwbuf->type == BFS_ERROR && !(state->flags & BFTW_RECOVER)) {
		state->error = ftwbuf->error;
		return BFTW_STOP;
	}

	enum bftw_action ret = BFTW_PRUNE;
	if ((state->flags & BFTW_SKIP_MOUNTS) && bftw_is_mount(state, name)) {
		goto done;
	}

	ret = state->callback(ftwbuf, state->ptr);
	switch (ret) {
	case BFTW_CONTINUE:
		if (visit != BFTW_PRE || ftwbuf->type != BFS_DIR) {
			ret = BFTW_PRUNE;
		} else if (state->flags & BFTW_PRUNE_MOUNTS) {
			if (bftw_is_mount(state, name)) {
				ret = BFTW_PRUNE;
			}
		}
		break;

	case BFTW_PRUNE:
	case BFTW_STOP:
		break;

	default:
		state->error = EINVAL;
		return BFTW_STOP;
	}

done:
	if (state->fileq.flags & BFTW_QBALANCE) {
		// Detect any main-thread stat() calls to rebalance the queue
		const struct bfs_stat *buf = bftw_cached_stat(ftwbuf, BFS_STAT_FOLLOW);
		const struct bfs_stat *lbuf = bftw_cached_stat(ftwbuf, BFS_STAT_NOFOLLOW);
		if (bftw_stat_was_sync(state, buf) || bftw_stat_was_sync(state, lbuf)) {
			bftw_queue_rebalance(&state->fileq, false);
		}
	}

	return ret;
}

/**
 * Flags controlling which files get visited when done with a directory.
 */
enum bftw_gc_flags {
	/** Don't visit anything. */
	BFTW_VISIT_NONE = 0,
	/** Report directory errors. */
	BFTW_VISIT_ERROR = 1 << 0,
	/** Visit the file itself. */
	BFTW_VISIT_FILE = 1 << 1,
	/** Visit the file's ancestors. */
	BFTW_VISIT_PARENTS = 1 << 2,
	/** Visit both the file and its ancestors. */
	BFTW_VISIT_ALL = BFTW_VISIT_ERROR | BFTW_VISIT_FILE | BFTW_VISIT_PARENTS,
};

/** Garbage collect the current file and its parents. */
static int bftw_gc(struct bftw_state *state, enum bftw_gc_flags flags) {
	int ret = 0;

	struct bftw_file *file = state->file;
	if (file) {
		if (state->dir) {
			bftw_cache_unpin(&state->cache, file);
		}
		if (file->dir) {
			bftw_delayed_unwrap(state, file);
		}
	}
	state->dir = NULL;
	state->de = NULL;

	if (state->direrror != 0) {
		if (flags & BFTW_VISIT_ERROR) {
			if (bftw_call_back(state, NULL, BFTW_PRE) == BFTW_STOP) {
				ret = -1;
				flags = 0;
			}
		} else {
			state->error = state->direrror;
		}
	}
	state->direrror = 0;

	while ((file = SLIST_POP(&state->to_close, ready))) {
		bftw_unwrapdir(state, file);
	}

	enum bftw_gc_flags visit = BFTW_VISIT_FILE;
	while ((file = state->file)) {
		if (--file->refcount > 0) {
			state->file = NULL;
			break;
		}

		if (flags & visit) {
			if (bftw_call_back(state, NULL, BFTW_POST) == BFTW_STOP) {
				ret = -1;
				flags = 0;
			}
		}
		visit = BFTW_VISIT_PARENTS;

		struct bftw_file *parent = file->parent;
		if (state->previous == file) {
			state->previous = parent;
		}
		state->file = parent;

		if (file->fd >= 0) {
			bftw_close(state, file);
		}
		bftw_file_free(&state->cache, file);
	}

	return ret;
}

/** Sort a bftw_list by filename. */
static void bftw_list_sort(struct bftw_list *list) {
	if (!list->head || !list->head->next) {
		return;
	}

	struct bftw_list left, right;
	SLIST_INIT(&left);
	SLIST_INIT(&right);

	// Split
	for (struct bftw_file *hare = list->head; hare && (hare = hare->next); hare = hare->next) {
		struct bftw_file *tortoise = SLIST_POP(list);
		SLIST_APPEND(&left, tortoise);
	}
	SLIST_EXTEND(&right, list);

	// Recurse
	bftw_list_sort(&left);
	bftw_list_sort(&right);

	// Merge
	while (!SLIST_EMPTY(&left) && !SLIST_EMPTY(&right)) {
		struct bftw_file *lf = left.head;
		struct bftw_file *rf = right.head;

		if (strcoll(lf->name, rf->name) <= 0) {
			SLIST_POP(&left);
			SLIST_APPEND(list, lf);
		} else {
			SLIST_POP(&right);
			SLIST_APPEND(list, rf);
		}
	}
	SLIST_EXTEND(list, &left);
	SLIST_EXTEND(list, &right);
}

/** Flush all the queue buffers. */
static void bftw_flush(struct bftw_state *state) {
	if (state->flags & BFTW_SORT) {
		bftw_list_sort(&state->fileq.buffer);
	}
	bftw_queue_flush(&state->fileq);
	bftw_stat_files(state);

	bftw_queue_flush(&state->dirq);
	bftw_ioq_opendirs(state);
}

/** Close the current directory. */
static int bftw_closedir(struct bftw_state *state) {
	if (bftw_gc(state, BFTW_VISIT_ALL) != 0) {
		return -1;
	}

	bftw_flush(state);
	return 0;
}

/** Fill file identity information from an ftwbuf. */
static void bftw_save_ftwbuf(struct bftw_file *file, const struct BFTW *ftwbuf) {
	file->type = ftwbuf->type;

	const struct bfs_stat *statbuf = bftw_cached_stat(ftwbuf, ftwbuf->stat_flags);
	if (statbuf) {
		file->dev = statbuf->dev;
		file->ino = statbuf->ino;
	}
}

/** Check if we should buffer a file instead of visiting it. */
static bool bftw_buffer_file(const struct bftw_state *state, const struct bftw_file *file, const char *name) {
	if (!name) {
		// Already buffered
		return false;
	}

	if (state->flags & BFTW_BUFFER) {
		return true;
	}

	// If we need to call stat(), and can do it async, buffer this file
	if (!state->ioq) {
		return false;
	}

	if (!bftw_queue_balanced(&state->fileq)) {
		// stat() would run synchronously anyway
		return false;
	}

	size_t depth = file ? file->depth + 1 : 1;
	enum bfs_type type = state->de ? state->de->type : BFS_UNKNOWN;
	return bftw_must_stat(state, depth, type, name);
}

/** Visit and/or enqueue the current file. */
static int bftw_visit(struct bftw_state *state, const char *name) {
	struct bftw_cache *cache = &state->cache;
	struct bftw_file *file = state->file;

	if (bftw_buffer_file(state, file, name)) {
		file = bftw_file_new(cache, file, name);
		if (!file) {
			state->error = errno;
			return -1;
		}

		if (state->de) {
			file->type = state->de->type;
		}

		bftw_push_file(state, file);
		return 0;
	}

	switch (bftw_call_back(state, name, BFTW_PRE)) {
	case BFTW_CONTINUE:
		if (name) {
			file = bftw_file_new(cache, state->file, name);
		} else {
			state->file = NULL;
		}
		if (!file) {
			state->error = errno;
			return -1;
		}

		bftw_save_ftwbuf(file, &state->ftwbuf);
		bftw_stat_recycle(cache, file);
		bftw_push_dir(state, file);
		return 0;

	case BFTW_PRUNE:
		if (file && !name) {
			return bftw_gc(state, BFTW_VISIT_PARENTS);
		} else {
			return 0;
		}

	default:
		if (file && !name) {
			bftw_gc(state, BFTW_VISIT_NONE);
		}
		return -1;
	}
}

/** Drain a bftw_queue. */
static void bftw_drain(struct bftw_state *state, struct bftw_queue *queue) {
	bftw_queue_flush(queue);

	while (bftw_pop(state, queue)) {
		bftw_gc(state, BFTW_VISIT_NONE);
	}
}

/**
 * Dispose of the bftw() state.
 *
 * @return
 *         The bftw() return value.
 */
static int bftw_state_destroy(struct bftw_state *state) {
	dstrfree(state->path);

	struct ioq *ioq = state->ioq;
	if (ioq) {
		ioq_cancel(ioq);
		while (bftw_ioq_pop(state, true) >= 0);
		state->ioq = NULL;
	}

	bftw_gc(state, BFTW_VISIT_NONE);
	bftw_drain(state, &state->dirq);
	bftw_drain(state, &state->fileq);

	ioq_destroy(ioq);

	bftw_cache_destroy(&state->cache);

	errno = state->error;
	return state->error ? -1 : 0;
}

/**
 * Shared implementation for all search strategies.
 */
static int bftw_impl(struct bftw_state *state) {
	for (size_t i = 0; i < state->npaths; ++i) {
		if (bftw_visit(state, state->paths[i]) != 0) {
			return -1;
		}
	}
	bftw_flush(state);

	while (true) {
		while (bftw_pop_dir(state)) {
			if (bftw_opendir(state) != 0) {
				return -1;
			}
			while (bftw_readdir(state) > 0) {
				if (bftw_visit(state, state->de->name) != 0) {
					return -1;
				}
			}
			if (bftw_closedir(state) != 0) {
				return -1;
			}
		}

		if (!bftw_pop_file(state)) {
			break;
		}
		if (bftw_visit(state, NULL) != 0) {
			return -1;
		}
		bftw_flush(state);
	}

	return 0;
}

/**
 * bftw() implementation for simple breadth-/depth-first search.
 */
static int bftw_walk(const struct bftw_args *args) {
	struct bftw_state state;
	if (bftw_state_init(&state, args) != 0) {
		return -1;
	}

	bftw_impl(&state);
	return bftw_state_destroy(&state);
}

/**
 * Iterative deepening search state.
 */
struct bftw_ids_state {
	/** Nested walk state. */
	struct bftw_state nested;
	/** The wrapped callback. */
	bftw_callback *delegate;
	/** The wrapped callback arguments. */
	void *ptr;
	/** Which visit this search corresponds to. */
	enum bftw_visit visit;
	/** Whether to override the bftw_visit. */
	bool force_visit;
	/** The current minimum depth (inclusive). */
	size_t min_depth;
	/** The current maximum depth (exclusive). */
	size_t max_depth;
	/** The set of pruned paths. */
	struct trie pruned;
	/** Whether the bottom has been found. */
	bool bottom;
};

/** Iterative deepening callback function. */
static enum bftw_action bftw_ids_callback(const struct BFTW *ftwbuf, void *ptr) {
	struct bftw_ids_state *state = ptr;

	if (state->force_visit) {
		struct BFTW *mutbuf = (struct BFTW *)ftwbuf;
		mutbuf->visit = state->visit;
	}

	if (ftwbuf->type == BFS_ERROR) {
		if (ftwbuf->depth + 1 >= state->min_depth) {
			return state->delegate(ftwbuf, state->ptr);
		} else {
			return BFTW_PRUNE;
		}
	}

	if (ftwbuf->depth < state->min_depth) {
		if (trie_find_str(&state->pruned, ftwbuf->path)) {
			return BFTW_PRUNE;
		} else {
			return BFTW_CONTINUE;
		}
	} else if (state->visit == BFTW_POST) {
		if (trie_find_str(&state->pruned, ftwbuf->path)) {
			return BFTW_PRUNE;
		}
	}

	enum bftw_action ret = BFTW_CONTINUE;
	if (ftwbuf->visit == state->visit) {
		ret = state->delegate(ftwbuf, state->ptr);
	}

	switch (ret) {
	case BFTW_CONTINUE:
		if (ftwbuf->type == BFS_DIR && ftwbuf->depth + 1 >= state->max_depth) {
			state->bottom = false;
			ret = BFTW_PRUNE;
		}
		break;

	case BFTW_PRUNE:
		if (ftwbuf->type == BFS_DIR) {
			if (!trie_insert_str(&state->pruned, ftwbuf->path)) {
				state->nested.error = errno;
				ret = BFTW_STOP;
			}
		}
		break;

	case BFTW_STOP:
		break;
	}

	return ret;
}

/** Initialize iterative deepening state. */
static int bftw_ids_init(struct bftw_ids_state *state, const struct bftw_args *args) {
	state->delegate = args->callback;
	state->ptr = args->ptr;
	state->visit = BFTW_PRE;
	state->force_visit = false;
	state->min_depth = 0;
	state->max_depth = 1;
	trie_init(&state->pruned);
	state->bottom = false;

	struct bftw_args ids_args = *args;
	ids_args.callback = bftw_ids_callback;
	ids_args.ptr = state;
	ids_args.flags &= ~BFTW_POST_ORDER;
	return bftw_state_init(&state->nested, &ids_args);
}

/** Finish an iterative deepening search. */
static int bftw_ids_destroy(struct bftw_ids_state *state) {
	trie_destroy(&state->pruned);
	return bftw_state_destroy(&state->nested);
}

/**
 * Iterative deepening bftw() wrapper.
 */
static int bftw_ids(const struct bftw_args *args) {
	struct bftw_ids_state state;
	if (bftw_ids_init(&state, args) != 0) {
		return -1;
	}

	while (!state.bottom) {
		state.bottom = true;

		if (bftw_impl(&state.nested) != 0) {
			goto done;
		}

		++state.min_depth;
		++state.max_depth;
	}

	if (args->flags & BFTW_POST_ORDER) {
		state.visit = BFTW_POST;
		state.force_visit = true;

		while (state.min_depth > 0) {
			--state.max_depth;
			--state.min_depth;

			if (bftw_impl(&state.nested) != 0) {
				goto done;
			}
		}
	}

done:
	return bftw_ids_destroy(&state);
}

/**
 * Exponential deepening bftw() wrapper.
 */
static int bftw_eds(const struct bftw_args *args) {
	struct bftw_ids_state state;
	if (bftw_ids_init(&state, args) != 0) {
		return -1;
	}

	while (!state.bottom) {
		state.bottom = true;

		if (bftw_impl(&state.nested) != 0) {
			goto done;
		}

		state.min_depth = state.max_depth;
		state.max_depth *= 2;
	}

	if (args->flags & BFTW_POST_ORDER) {
		state.visit = BFTW_POST;
		state.min_depth = 0;
		state.nested.flags |= BFTW_POST_ORDER;

		bftw_impl(&state.nested);
	}

done:
	return bftw_ids_destroy(&state);
}

int bftw(const struct bftw_args *args) {
	switch (args->strategy) {
	case BFTW_BFS:
	case BFTW_DFS:
		return bftw_walk(args);
	case BFTW_IDS:
		return bftw_ids(args);
	case BFTW_EDS:
		return bftw_eds(args);
	}

	errno = EINVAL;
	return -1;
}
