// Copyright © Tavian Barnes <tavianator@tavianator.com>
// SPDX-License-Identifier: 0BSD

#include "prelude.h"
#include "xregex.h"
#include "alloc.h"
#include "bfstd.h"
#include "diag.h"
#include "sanity.h"
#include "thread.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if BFS_USE_ONIGURUMA
#  include <langinfo.h>
#  include <oniguruma.h>
#else
#  include <regex.h>
#endif

struct bfs_regex {
#if BFS_USE_ONIGURUMA
	unsigned char *pattern;
	OnigRegex impl;
	int err;
	OnigErrorInfo einfo;
#else
	regex_t impl;
	int err;
#endif
};

#if BFS_USE_ONIGURUMA

static int bfs_onig_status;
static OnigEncoding bfs_onig_enc;

/** pthread_once() callback. */
static void bfs_onig_once(void) {
	// Fall back to ASCII by default
	bfs_onig_enc = ONIG_ENCODING_ASCII;

	// Oniguruma has no locale support, so try to guess the right encoding
	// from the current locale.
	const char *charmap = nl_langinfo(CODESET);
	if (charmap) {
#define BFS_MAP_ENCODING(name, value) \
		do { \
			if (strcmp(charmap, name) == 0) { \
				bfs_onig_enc = value; \
			} \
		} while (0)
#define BFS_MAP_ENCODING2(name1, name2, value) \
		do { \
			BFS_MAP_ENCODING(name1, value); \
			BFS_MAP_ENCODING(name2, value); \
		} while (0)

		// These names were found with locale -m on Linux and FreeBSD
#define BFS_MAP_ISO_8859(n) \
		BFS_MAP_ENCODING2("ISO-8859-" #n, "ISO8859-" #n, ONIG_ENCODING_ISO_8859_ ## n)

		BFS_MAP_ISO_8859(1);
		BFS_MAP_ISO_8859(2);
		BFS_MAP_ISO_8859(3);
		BFS_MAP_ISO_8859(4);
		BFS_MAP_ISO_8859(5);
		BFS_MAP_ISO_8859(6);
		BFS_MAP_ISO_8859(7);
		BFS_MAP_ISO_8859(8);
		BFS_MAP_ISO_8859(9);
		BFS_MAP_ISO_8859(10);
		BFS_MAP_ISO_8859(11);
		// BFS_MAP_ISO_8859(12);
		BFS_MAP_ISO_8859(13);
		BFS_MAP_ISO_8859(14);
		BFS_MAP_ISO_8859(15);
		BFS_MAP_ISO_8859(16);

		BFS_MAP_ENCODING("UTF-8", ONIG_ENCODING_UTF8);

#define BFS_MAP_EUC(name) \
		BFS_MAP_ENCODING2("EUC-" #name, "euc" #name, ONIG_ENCODING_EUC_ ## name)

		BFS_MAP_EUC(JP);
		BFS_MAP_EUC(TW);
		BFS_MAP_EUC(KR);
		BFS_MAP_EUC(CN);

		BFS_MAP_ENCODING2("SHIFT_JIS", "SJIS", ONIG_ENCODING_SJIS);

		// BFS_MAP_ENCODING("KOI-8", ONIG_ENCODING_KOI8);
		BFS_MAP_ENCODING("KOI8-R", ONIG_ENCODING_KOI8_R);

		BFS_MAP_ENCODING("CP1251", ONIG_ENCODING_CP1251);

		BFS_MAP_ENCODING("GB18030", ONIG_ENCODING_BIG5);
	}

	bfs_onig_status = onig_initialize(&bfs_onig_enc, 1);
	if (bfs_onig_status != ONIG_NORMAL) {
		bfs_onig_enc = NULL;
	}
}

/** Initialize Oniguruma. */
static int bfs_onig_initialize(OnigEncoding *enc) {
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	invoke_once(&once, bfs_onig_once);

	*enc = bfs_onig_enc;
	return bfs_onig_status;
}
#endif

int bfs_regcomp(struct bfs_regex **preg, const char *pattern, enum bfs_regex_type type, enum bfs_regcomp_flags flags) {
	struct bfs_regex *regex = *preg = ALLOC(struct bfs_regex);
	if (!regex) {
		return -1;
	}

#if BFS_USE_ONIGURUMA
	// onig_error_code_to_str() says
	//
	//     don't call this after the pattern argument of onig_new() is freed
	//
	// so make a defensive copy.
	regex->pattern = (unsigned char *)strdup(pattern);
	if (!regex->pattern) {
		goto fail;
	}

	regex->impl = NULL;
	regex->err = ONIG_NORMAL;

	OnigSyntaxType *syntax = NULL;
	switch (type) {
	case BFS_REGEX_POSIX_BASIC:
		syntax = ONIG_SYNTAX_POSIX_BASIC;
		break;
	case BFS_REGEX_POSIX_EXTENDED:
		syntax = ONIG_SYNTAX_POSIX_EXTENDED;
		break;
	case BFS_REGEX_EMACS:
		syntax = ONIG_SYNTAX_EMACS;
		break;
	case BFS_REGEX_GREP:
		syntax = ONIG_SYNTAX_GREP;
		break;
	}
	bfs_assert(syntax, "Invalid regex type");

	OnigOptionType options = syntax->options;
	if (flags & BFS_REGEX_ICASE) {
		options |= ONIG_OPTION_IGNORECASE;
	}

	OnigEncoding enc;
	regex->err = bfs_onig_initialize(&enc);
	if (regex->err != ONIG_NORMAL) {
		return -1;
	}

	const unsigned char *end = regex->pattern + strlen(pattern);
	regex->err = onig_new(&regex->impl, regex->pattern, end, options, enc, syntax, &regex->einfo);
	if (regex->err != ONIG_NORMAL) {
		return -1;
	}
#else
	int cflags = 0;
	switch (type) {
	case BFS_REGEX_POSIX_BASIC:
		break;
	case BFS_REGEX_POSIX_EXTENDED:
		cflags |= REG_EXTENDED;
		break;
	default:
		errno = EINVAL;
		goto fail;
	}

	if (flags & BFS_REGEX_ICASE) {
		cflags |= REG_ICASE;
	}

	regex->err = regcomp(&regex->impl, pattern, cflags);
	if (regex->err != 0) {
		// https://github.com/google/sanitizers/issues/1496
		sanitize_init(&regex->impl);
		return -1;
	}
#endif

	return 0;

fail:
	free(regex);
	*preg = NULL;
	return -1;
}

int bfs_regexec(struct bfs_regex *regex, const char *str, enum bfs_regexec_flags flags) {
	size_t len = strlen(str);

#if BFS_USE_ONIGURUMA
	const unsigned char *ustr = (const unsigned char *)str;
	const unsigned char *end = ustr + len;

	// The docs for onig_{match,search}() say
	//
	//     Do not pass invalid byte string in the regex character encoding.
	if (!onigenc_is_valid_mbc_string(onig_get_encoding(regex->impl), ustr, end)) {
		return 0;
	}

	int ret;
	if (flags & BFS_REGEX_ANCHOR) {
		ret = onig_match(regex->impl, ustr, end, ustr, NULL, ONIG_OPTION_DEFAULT);
	} else {
		ret = onig_search(regex->impl, ustr, end, ustr, end, NULL, ONIG_OPTION_DEFAULT);
	}

	if (ret >= 0) {
		if (flags & BFS_REGEX_ANCHOR) {
			return (size_t)ret == len;
		} else {
			return 1;
		}
	} else if (ret == ONIG_MISMATCH) {
		return 0;
	} else {
		regex->err = ret;
		return -1;
	}
#else
	regmatch_t match = {
		.rm_so = 0,
		.rm_eo = len,
	};

	int eflags = 0;
#ifdef REG_STARTEND
	eflags |= REG_STARTEND;
#endif

	int ret = regexec(&regex->impl, str, 1, &match, eflags);
	if (ret == 0) {
		if (flags & BFS_REGEX_ANCHOR) {
			return match.rm_so == 0 && (size_t)match.rm_eo == len;
		} else {
			return 1;
		}
	} else if (ret == REG_NOMATCH) {
		return 0;
	} else {
		regex->err = ret;
		return -1;
	}
#endif
}

void bfs_regfree(struct bfs_regex *regex) {
	if (regex) {
#if BFS_USE_ONIGURUMA
		onig_free(regex->impl);
		free(regex->pattern);
#else
		regfree(&regex->impl);
#endif
		free(regex);
	}
}

char *bfs_regerror(const struct bfs_regex *regex) {
	if (!regex) {
		return strdup(xstrerror(ENOMEM));
	}

#if BFS_USE_ONIGURUMA
	unsigned char *str = malloc(ONIG_MAX_ERROR_MESSAGE_LEN);
	if (str) {
		onig_error_code_to_str(str, regex->err, &regex->einfo);
	}
	return (char *)str;
#else
	size_t len = regerror(regex->err, &regex->impl, NULL, 0);
	char *str = malloc(len);
	if (str) {
		regerror(regex->err, &regex->impl, str, len);
	}
	return str;
#endif
}
