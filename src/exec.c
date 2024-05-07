// Copyright © Tavian Barnes <tavianator@tavianator.com>
// SPDX-License-Identifier: 0BSD

#include "prelude.h"
#include "exec.h"
#include "alloc.h"
#include "bfstd.h"
#include "bftw.h"
#include "color.h"
#include "ctx.h"
#include "diag.h"
#include "dstring.h"
#include "xspawn.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/** Print some debugging info. */
attr(printf(2, 3))
static void bfs_exec_debug(const struct bfs_exec *execbuf, const char *format, ...) {
	const struct bfs_ctx *ctx = execbuf->ctx;

	if (!bfs_debug(ctx, DEBUG_EXEC, "${blu}")) {
		return;
	}

	if (execbuf->flags & BFS_EXEC_CONFIRM) {
		fputs("-ok", stderr);
	} else {
		fputs("-exec", stderr);
	}
	if (execbuf->flags & BFS_EXEC_CHDIR) {
		fputs("dir", stderr);
	}
	cfprintf(ctx->cerr, "${rs}: ");

	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

/** Determine the size of a single argument, for comparison to arg_max. */
static size_t bfs_exec_arg_size(const char *arg) {
	return sizeof(arg) + strlen(arg) + 1;
}

/** Even if we can pass a bigger argument list, cap it here. */
#define BFS_EXEC_ARG_MAX (16 << 20)

/** Determine the maximum argv size. */
static size_t bfs_exec_arg_max(const struct bfs_exec *execbuf) {
	long arg_max = sysconf(_SC_ARG_MAX);
	bfs_exec_debug(execbuf, "ARG_MAX: %ld according to sysconf()\n", arg_max);
	if (arg_max < 0) {
		arg_max = BFS_EXEC_ARG_MAX;
		bfs_exec_debug(execbuf, "ARG_MAX: %ld assumed\n", arg_max);
	}

	// We have to share space with the environment variables
	extern char **environ;
	for (char **envp = environ; *envp; ++envp) {
		arg_max -= bfs_exec_arg_size(*envp);
	}
	// Account for the terminating NULL entry
	arg_max -= sizeof(char *);
	bfs_exec_debug(execbuf, "ARG_MAX: %ld remaining after environment variables\n", arg_max);

	// Account for the fixed arguments
	for (size_t i = 0; i < execbuf->tmpl_argc - 1; ++i) {
		arg_max -= bfs_exec_arg_size(execbuf->tmpl_argv[i]);
	}
	// Account for the terminating NULL entry
	arg_max -= sizeof(char *);
	bfs_exec_debug(execbuf, "ARG_MAX: %ld remaining after fixed arguments\n", arg_max);

	// Assume arguments are counted with the granularity of a single page,
	// so allow a one page cushion to account for rounding up
	long page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 4096) {
		page_size = 4096;
	}
	arg_max -= page_size;
	bfs_exec_debug(execbuf, "ARG_MAX: %ld remaining after page cushion\n", arg_max);

	// POSIX recommends an additional 2048 bytes of headroom
	arg_max -= 2048;
	bfs_exec_debug(execbuf, "ARG_MAX: %ld remaining after headroom\n", arg_max);

	if (arg_max < 0) {
		arg_max = 0;
	} else if (arg_max > BFS_EXEC_ARG_MAX) {
		arg_max = BFS_EXEC_ARG_MAX;
	}

	bfs_exec_debug(execbuf, "ARG_MAX: %ld final value\n", arg_max);
	return arg_max;
}

/** Highlight part of the command line as an error. */
static void bfs_exec_parse_error(const struct bfs_ctx *ctx, const struct bfs_exec *execbuf) {
	char **argv = execbuf->tmpl_argv - 1;
	size_t argc = execbuf->tmpl_argc + 1;
	if (argv[argc]) {
		++argc;
	}

	bool args[ctx->argc];
	for (size_t i = 0; i < ctx->argc; ++i) {
		args[i] = false;
	}

	size_t i = argv - ctx->argv;
	for (size_t j = 0; j < argc; ++j) {
		args[i + j] = true;
	}

	bfs_argv_error(ctx, args);
}

struct bfs_exec *bfs_exec_parse(const struct bfs_ctx *ctx, char **argv, enum bfs_exec_flags flags) {
	struct bfs_exec *execbuf = ZALLOC(struct bfs_exec);
	if (!execbuf) {
		bfs_perror(ctx, "zalloc()");
		goto fail;
	}

	execbuf->flags = flags;
	execbuf->ctx = ctx;
	execbuf->tmpl_argv = argv + 1;
	execbuf->wd_fd = -1;

	while (true) {
		const char *arg = execbuf->tmpl_argv[execbuf->tmpl_argc];
		if (!arg) {
			if (execbuf->flags & BFS_EXEC_CONFIRM) {
				bfs_exec_parse_error(ctx, execbuf);
				bfs_error(ctx, "Expected '... ;'.\n");
			} else {
				bfs_exec_parse_error(ctx, execbuf);
				bfs_error(ctx, "Expected '... ;' or '... {} +'.\n");
			}
			goto fail;
		} else if (strcmp(arg, ";") == 0) {
			break;
		} else if (execbuf->tmpl_argc > 0 && strcmp(arg, "+") == 0) {
			const char *prev = execbuf->tmpl_argv[execbuf->tmpl_argc - 1];
			if (!(execbuf->flags & BFS_EXEC_CONFIRM) && strcmp(prev, "{}") == 0) {
				execbuf->flags |= BFS_EXEC_MULTI;
				break;
			}
		}

		++execbuf->tmpl_argc;
	}

	if (execbuf->tmpl_argc == 0) {
		bfs_exec_parse_error(ctx, execbuf);
		bfs_error(ctx, "Missing command.\n");
		goto fail;
	}

	execbuf->argv_cap = execbuf->tmpl_argc + 1;
	execbuf->argv = ALLOC_ARRAY(char *, execbuf->argv_cap);
	if (!execbuf->argv) {
		bfs_perror(ctx, "alloc()");
		goto fail;
	}

	if (execbuf->flags & BFS_EXEC_MULTI) {
		for (size_t i = 0; i < execbuf->tmpl_argc - 1; ++i) {
			char *arg = execbuf->tmpl_argv[i];
			if (strstr(arg, "{}")) {
				bfs_exec_parse_error(ctx, execbuf);
				bfs_error(ctx, "Only one '{}' is supported.\n");
				goto fail;
			}
			execbuf->argv[i] = arg;
		}
		execbuf->argc = execbuf->tmpl_argc - 1;

		execbuf->arg_max = bfs_exec_arg_max(execbuf);
		execbuf->arg_min = execbuf->arg_max;
	}

	return execbuf;

fail:
	bfs_exec_free(execbuf);
	return NULL;
}

/** Format the current path for use as a command line argument. */
static char *bfs_exec_format_path(const struct bfs_exec *execbuf, const struct BFTW *ftwbuf) {
	if (!(execbuf->flags & BFS_EXEC_CHDIR)) {
		return strdup(ftwbuf->path);
	}

	const char *name = ftwbuf->path + ftwbuf->nameoff;

	if (name[0] == '/') {
		// Must be a root path ("/", "//", etc.)
		return strdup(name);
	}

	// For compatibility with GNU find, use './name' instead of just 'name'
	char *path = malloc(2 + strlen(name) + 1);
	if (!path) {
		return NULL;
	}

	char *cur = stpcpy(path, "./");
	cur = stpcpy(cur, name);
	return path;
}

/** Format an argument, expanding "{}" to the current path. */
static char *bfs_exec_format_arg(char *arg, const char *path) {
	char *match = strstr(arg, "{}");
	if (!match) {
		return arg;
	}

	dchar *ret = dstralloc(0);
	if (!ret) {
		return NULL;
	}

	char *last = arg;
	do {
		if (dstrncat(&ret, last, match - last) != 0) {
			goto err;
		}
		if (dstrcat(&ret, path) != 0) {
			goto err;
		}

		last = match + 2;
		match = strstr(last, "{}");
	} while (match);

	if (dstrcat(&ret, last) != 0) {
		goto err;
	}

	return ret;

err:
	dstrfree(ret);
	return NULL;
}

/** Free a formatted argument. */
static void bfs_exec_free_arg(char *arg, const char *tmpl) {
	if (arg != tmpl) {
		dstrfree((dchar *)arg);
	}
}

/** Open a file to use as the working directory. */
static int bfs_exec_openwd(struct bfs_exec *execbuf, const struct BFTW *ftwbuf) {
	bfs_assert(execbuf->wd_fd < 0);
	bfs_assert(!execbuf->wd_path);

	if (ftwbuf->at_fd != AT_FDCWD) {
		// Rely on at_fd being the immediate parent
		bfs_assert(xbaseoff(ftwbuf->at_path) == 0);

		execbuf->wd_fd = ftwbuf->at_fd;
		if (!(execbuf->flags & BFS_EXEC_MULTI)) {
			return 0;
		}

		execbuf->wd_fd = dup_cloexec(execbuf->wd_fd);
		if (execbuf->wd_fd < 0) {
			return -1;
		}
	}

	execbuf->wd_len = ftwbuf->nameoff;
	if (execbuf->wd_len == 0) {
		if (ftwbuf->path[0] == '/') {
			++execbuf->wd_len;
		} else {
			// The path is something like "foo", so we're already in the right directory
			return 0;
		}
	}

	execbuf->wd_path = strndup(ftwbuf->path, execbuf->wd_len);
	if (!execbuf->wd_path) {
		return -1;
	}

	if (execbuf->wd_fd < 0) {
		execbuf->wd_fd = open(execbuf->wd_path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
	}

	if (execbuf->wd_fd < 0) {
		return -1;
	}

	return 0;
}

/** Close the working directory. */
static void bfs_exec_closewd(struct bfs_exec *execbuf, const struct BFTW *ftwbuf) {
	if (execbuf->wd_fd >= 0) {
		if (!ftwbuf || execbuf->wd_fd != ftwbuf->at_fd) {
			xclose(execbuf->wd_fd);
		}
		execbuf->wd_fd = -1;
	}

	if (execbuf->wd_path) {
		free(execbuf->wd_path);
		execbuf->wd_path = NULL;
		execbuf->wd_len = 0;
	}
}

/** Actually spawn the process. */
static int bfs_exec_spawn(const struct bfs_exec *execbuf) {
	const struct bfs_ctx *ctx = execbuf->ctx;

	// Flush the context state for consistency with the external process
	bfs_ctx_flush(ctx);

	if (execbuf->flags & BFS_EXEC_CONFIRM) {
		for (size_t i = 0; i < execbuf->argc; ++i) {
			if (fprintf(stderr, "%s ", execbuf->argv[i]) < 0) {
				return -1;
			}
		}
		if (fprintf(stderr, "? ") < 0) {
			return -1;
		}

		if (ynprompt() <= 0) {
			errno = 0;
			return -1;
		}
	}

	if (execbuf->flags & BFS_EXEC_MULTI) {
		bfs_exec_debug(execbuf, "Executing '%s' ... [%zu arguments] (size %zu)\n",
			execbuf->argv[0], execbuf->argc - 1, execbuf->arg_size);
	} else {
		bfs_exec_debug(execbuf, "Executing '%s' ... [%zu arguments]\n", execbuf->argv[0], execbuf->argc - 1);
	}

	pid_t pid = -1;

	struct bfs_spawn spawn;
	if (bfs_spawn_init(&spawn) != 0) {
		return -1;
	}

	spawn.flags |= BFS_SPAWN_USE_PATH;

	if (execbuf->wd_fd >= 0) {
		if (bfs_spawn_addfchdir(&spawn, execbuf->wd_fd) != 0) {
			goto fail;
		}
	}

	// Reset RLIMIT_NOFILE if necessary, to avoid breaking applications that use select()
	if (rlim_cmp(ctx->orig_nofile.rlim_cur, ctx->cur_nofile.rlim_cur) < 0) {
		if (bfs_spawn_setrlimit(&spawn, RLIMIT_NOFILE, &ctx->orig_nofile) != 0) {
			goto fail;
		}
	}

	pid = bfs_spawn(execbuf->argv[0], &spawn, execbuf->argv, NULL);

fail:;
	int error = errno;

	bfs_spawn_destroy(&spawn);
	if (pid < 0) {
		errno = error;
		return -1;
	}

	int wstatus;
	if (xwaitpid(pid, &wstatus, 0) < 0) {
		return -1;
	}

	int ret = -1;

	if (WIFEXITED(wstatus)) {
		int status = WEXITSTATUS(wstatus);
		if (status == EXIT_SUCCESS) {
			ret = 0;
		} else {
			bfs_exec_debug(execbuf, "Command '%s' failed with status %d\n", execbuf->argv[0], status);
		}
	} else if (WIFSIGNALED(wstatus)) {
		int sig = WTERMSIG(wstatus);
		const char *str = strsignal(sig);
		if (!str) {
			str = "unknown";
		}
		bfs_warning(ctx, "Command '${ex}%s${rs}' terminated by signal %d (%s)\n", execbuf->argv[0], sig, str);
	} else {
		bfs_warning(ctx, "Command '${ex}%s${rs}' terminated abnormally\n", execbuf->argv[0]);
	}

	errno = 0;
	return ret;
}

/** exec() a command for a single file. */
static int bfs_exec_single(struct bfs_exec *execbuf, const struct BFTW *ftwbuf) {
	int ret = -1, error = 0;

	char *path = bfs_exec_format_path(execbuf, ftwbuf);
	if (!path) {
		goto out;
	}

	size_t i;
	for (i = 0; i < execbuf->tmpl_argc; ++i) {
		execbuf->argv[i] = bfs_exec_format_arg(execbuf->tmpl_argv[i], path);
		if (!execbuf->argv[i]) {
			goto out_free;
		}
	}
	execbuf->argv[i] = NULL;
	execbuf->argc = i;

	if (execbuf->flags & BFS_EXEC_CHDIR) {
		if (bfs_exec_openwd(execbuf, ftwbuf) != 0) {
			goto out_free;
		}
	}

	ret = bfs_exec_spawn(execbuf);

out_free:
	error = errno;

	bfs_exec_closewd(execbuf, ftwbuf);

	for (size_t j = 0; j < i; ++j) {
		bfs_exec_free_arg(execbuf->argv[j], execbuf->tmpl_argv[j]);
	}

	free(path);

	errno = error;

out:
	return ret;
}

/** Check if any arguments remain in the buffer. */
static bool bfs_exec_args_remain(const struct bfs_exec *execbuf) {
	return execbuf->argc >= execbuf->tmpl_argc;
}

/** Compute the current ARG_MAX estimate for binary search. */
static size_t bfs_exec_estimate_max(const struct bfs_exec *execbuf) {
	size_t min = execbuf->arg_min;
	size_t max = execbuf->arg_max;
	return min + (max - min) / 2;
}

/** Update the ARG_MAX lower bound from a successful execution. */
static void bfs_exec_update_min(struct bfs_exec *execbuf) {
	if (execbuf->arg_size > execbuf->arg_min) {
		execbuf->arg_min = execbuf->arg_size;

		// Don't let min exceed max
		if (execbuf->arg_min > execbuf->arg_max) {
			execbuf->arg_min = execbuf->arg_max;
		}

		size_t estimate = bfs_exec_estimate_max(execbuf);
		bfs_exec_debug(execbuf, "ARG_MAX between [%zu, %zu], trying %zu\n",
			execbuf->arg_min, execbuf->arg_max, estimate);
	}
}

/** Update the ARG_MAX upper bound from a failed execution. */
static size_t bfs_exec_update_max(struct bfs_exec *execbuf) {
	bfs_exec_debug(execbuf, "Got E2BIG, shrinking argument list...\n");

	size_t size = execbuf->arg_size;
	if (size <= execbuf->arg_min) {
		// Lower bound was wrong, restart binary search.
		execbuf->arg_min = 0;
	}

	// Trim a fraction off the max size to avoid repeated failures near the
	// top end of the working range
	size -= size / 16;
	if (size < execbuf->arg_max) {
		execbuf->arg_max = size;

		// Don't let min exceed max
		if (execbuf->arg_min > execbuf->arg_max) {
			execbuf->arg_min = execbuf->arg_max;
		}
	}

	// Binary search for a more precise bound
	size_t estimate = bfs_exec_estimate_max(execbuf);
	bfs_exec_debug(execbuf, "ARG_MAX between [%zu, %zu], trying %zu\n",
		execbuf->arg_min, execbuf->arg_max, estimate);
	return estimate;
}

/** Execute the pending command from a BFS_EXEC_MULTI execbuf. */
static int bfs_exec_flush(struct bfs_exec *execbuf) {
	int ret = 0, error = 0;

	size_t orig_argc = execbuf->argc;
	while (bfs_exec_args_remain(execbuf)) {
		execbuf->argv[execbuf->argc] = NULL;
		ret = bfs_exec_spawn(execbuf);
		error = errno;
		if (ret == 0) {
			bfs_exec_update_min(execbuf);
			break;
		} else if (error != E2BIG) {
			break;
		}

		// Try to recover from E2BIG by trying fewer and fewer arguments
		// until they fit
		size_t new_max = bfs_exec_update_max(execbuf);
		while (execbuf->arg_size > new_max) {
			execbuf->argv[execbuf->argc] = execbuf->argv[execbuf->argc - 1];
			execbuf->arg_size -= bfs_exec_arg_size(execbuf->argv[execbuf->argc]);
			--execbuf->argc;
		}
	}

	size_t new_argc = execbuf->argc;
	for (size_t i = execbuf->tmpl_argc - 1; i < new_argc; ++i) {
		free(execbuf->argv[i]);
	}
	execbuf->argc = execbuf->tmpl_argc - 1;
	execbuf->arg_size = 0;

	if (new_argc < orig_argc) {
		// If we recovered from E2BIG, there are unused arguments at the
		// end of the list
		for (size_t i = new_argc + 1; i <= orig_argc; ++i) {
			if (error == 0) {
				execbuf->argv[execbuf->argc] = execbuf->argv[i];
				execbuf->arg_size += bfs_exec_arg_size(execbuf->argv[execbuf->argc]);
				++execbuf->argc;
			} else {
				free(execbuf->argv[i]);
			}
		}
	}

	errno = error;
	return ret;
}

/** Check if we need to flush the execbuf because we're changing directories. */
static bool bfs_exec_changed_dirs(const struct bfs_exec *execbuf, const struct BFTW *ftwbuf) {
	if (execbuf->flags & BFS_EXEC_CHDIR) {
		if (ftwbuf->nameoff > execbuf->wd_len
		    || (execbuf->wd_path && strncmp(ftwbuf->path, execbuf->wd_path, execbuf->wd_len) != 0)) {
			bfs_exec_debug(execbuf, "Changed directories, executing buffered command\n");
			return true;
		}
	}

	return false;
}

/** Check if we need to flush the execbuf because we're too big. */
static bool bfs_exec_would_overflow(const struct bfs_exec *execbuf, const char *arg) {
	size_t arg_max = bfs_exec_estimate_max(execbuf);
	size_t next_size = execbuf->arg_size + bfs_exec_arg_size(arg);
	if (next_size > arg_max) {
		bfs_exec_debug(execbuf, "Command size (%zu) would exceed maximum (%zu), executing buffered command\n",
			next_size, arg_max);
		return true;
	}

	return false;
}

/** Push a new argument to a BFS_EXEC_MULTI execbuf. */
static int bfs_exec_push(struct bfs_exec *execbuf, char *arg) {
	execbuf->argv[execbuf->argc] = arg;

	if (execbuf->argc + 1 >= execbuf->argv_cap) {
		size_t cap = 2 * execbuf->argv_cap;
		char **argv = REALLOC_ARRAY(char *, execbuf->argv, execbuf->argv_cap, cap);
		if (!argv) {
			return -1;
		}
		execbuf->argv = argv;
		execbuf->argv_cap = cap;
	}

	++execbuf->argc;
	execbuf->arg_size += bfs_exec_arg_size(arg);
	return 0;
}

/** Handle a new path for a BFS_EXEC_MULTI execbuf. */
static int bfs_exec_multi(struct bfs_exec *execbuf, const struct BFTW *ftwbuf) {
	int ret = 0;

	char *arg = bfs_exec_format_path(execbuf, ftwbuf);
	if (!arg) {
		ret = -1;
		goto out;
	}

	if (bfs_exec_changed_dirs(execbuf, ftwbuf)) {
		while (bfs_exec_args_remain(execbuf)) {
			ret |= bfs_exec_flush(execbuf);
		}
		bfs_exec_closewd(execbuf, ftwbuf);
	} else if (bfs_exec_would_overflow(execbuf, arg)) {
		ret |= bfs_exec_flush(execbuf);
	}

	if ((execbuf->flags & BFS_EXEC_CHDIR) && execbuf->wd_fd < 0) {
		if (bfs_exec_openwd(execbuf, ftwbuf) != 0) {
			ret = -1;
			goto out_arg;
		}
	}

	if (bfs_exec_push(execbuf, arg) != 0) {
		ret = -1;
		goto out_arg;
	}

	// arg will get cleaned up later by bfs_exec_flush()
	goto out;

out_arg:
	free(arg);
out:
	return ret;
}

int bfs_exec(struct bfs_exec *execbuf, const struct BFTW *ftwbuf) {
	if (execbuf->flags & BFS_EXEC_MULTI) {
		if (bfs_exec_multi(execbuf, ftwbuf) == 0) {
			errno = 0;
		} else {
			execbuf->ret = -1;
		}
		// -exec ... + never returns false
		return 0;
	} else {
		return bfs_exec_single(execbuf, ftwbuf);
	}
}

int bfs_exec_finish(struct bfs_exec *execbuf) {
	if (execbuf->flags & BFS_EXEC_MULTI) {
		bfs_exec_debug(execbuf, "Finishing execution, executing buffered command\n");
		while (bfs_exec_args_remain(execbuf)) {
			execbuf->ret |= bfs_exec_flush(execbuf);
		}
		if (execbuf->ret != 0) {
			bfs_exec_debug(execbuf, "One or more executions of '%s' failed\n", execbuf->argv[0]);
		}
	}
	return execbuf->ret;
}

void bfs_exec_free(struct bfs_exec *execbuf) {
	if (execbuf) {
		bfs_exec_closewd(execbuf, NULL);
		free(execbuf->argv);
		free(execbuf);
	}
}
