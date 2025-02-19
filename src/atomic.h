// Copyright © Tavian Barnes <tavianator@tavianator.com>
// SPDX-License-Identifier: 0BSD

/**
 * Shorthand for standard C atomic operations.
 */

#ifndef BFS_ATOMIC_H
#define BFS_ATOMIC_H

#include "bfs.h"

#include <stdatomic.h>

/**
 * Prettier spelling of _Atomic.
 */
#define atomic _Atomic

/**
 * Shorthand for atomic_load_explicit().
 *
 * @param obj
 *         A pointer to the atomic object.
 * @param order
 *         The memory ordering to use, without the memory_order_ prefix.
 * @return
 *         The loaded value.
 */
#define load(obj, order) \
	atomic_load_explicit(obj, memory_order_##order)

/**
 * Shorthand for atomic_store_explicit().
 */
#define store(obj, value, order) \
	atomic_store_explicit(obj, value, memory_order_##order)

/**
 * Shorthand for atomic_exchange_explicit().
 */
#define exchange(obj, value, order) \
	atomic_exchange_explicit(obj, value, memory_order_##order)

/**
 * Shorthand for atomic_compare_exchange_weak_explicit().
 */
#define compare_exchange_weak(obj, expected, desired, succ, fail) \
	atomic_compare_exchange_weak_explicit(obj, expected, desired, memory_order_##succ, memory_order_##fail)

/**
 * Shorthand for atomic_compare_exchange_strong_explicit().
 */
#define compare_exchange_strong(obj, expected, desired, succ, fail) \
	atomic_compare_exchange_strong_explicit(obj, expected, desired, memory_order_##succ, memory_order_##fail)

/**
 * Shorthand for atomic_fetch_add_explicit().
 */
#define fetch_add(obj, arg, order) \
	atomic_fetch_add_explicit(obj, arg, memory_order_##order)

/**
 * Shorthand for atomic_fetch_sub_explicit().
 */
#define fetch_sub(obj, arg, order) \
	atomic_fetch_sub_explicit(obj, arg, memory_order_##order)

/**
 * Shorthand for atomic_fetch_or_explicit().
 */
#define fetch_or(obj, arg, order) \
	atomic_fetch_or_explicit(obj, arg, memory_order_##order)

/**
 * Shorthand for atomic_fetch_xor_explicit().
 */
#define fetch_xor(obj, arg, order) \
	atomic_fetch_xor_explicit(obj, arg, memory_order_##order)

/**
 * Shorthand for atomic_fetch_and_explicit().
 */
#define fetch_and(obj, arg, order) \
	atomic_fetch_and_explicit(obj, arg, memory_order_##order)

/**
 * Shorthand for atomic_thread_fence().
 */
#if __SANITIZE_THREAD__
// TSan doesn't support fences: https://github.com/google/sanitizers/issues/1415
#  define thread_fence(obj, order) \
	fetch_add(obj, 0, order)
#else
#  define thread_fence(obj, order) \
	atomic_thread_fence(memory_order_##order)
#endif

/**
 * Shorthand for atomic_signal_fence().
 */
#define signal_fence(order) \
	atomic_signal_fence(memory_order_##order)

/**
 * A hint to the CPU to relax while it spins.
 */
#if __has_builtin(__builtin_ia32_pause)
#  define spin_loop() __builtin_ia32_pause()
#elif __has_builtin(__builtin_arm_yield)
#  define spin_loop() __builtin_arm_yield()
#elif BFS_HAS_BUILTIN_RISCV_PAUSE
#  define spin_loop() __builtin_riscv_pause()
#else
#  define spin_loop() ((void)0)
#endif

#endif // BFS_ATOMIC_H
