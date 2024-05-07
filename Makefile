# Copyright © Tavian Barnes <tavianator@tavianator.com>
# SPDX-License-Identifier: 0BSD

# To build bfs, run
#
#     $ ./configure
#     $ make

# Utilities and GNU/BSD portability
include build/prelude.mk

# The default build target
default: bfs
.PHONY: default

# Include the generated build config, if it exists
-include gen/config.mk

## Configuration phase (`./configure`)

# bfs used to have flag-like targets (`make release`, `make asan ubsan`, etc.).
# Direct users to the new configuration system.
asan lsan msan tsan ubsan gcov lint release::
	@printf 'error: `%s %s` is no longer supported. Use `./configure --enable-%s` instead.\n' \
	    "${MAKE}" $@ $@ >&2
	@false

# Print an error if `make` is run before `./configure`
gen/config.mk::
	@if ! [ -e $@ ]; then \
	    printf 'error: You must run `./configure` before `%s`.\n' "${MAKE}" >&2; \
	    false; \
	fi

## Build phase (`make`)

# The main binary
bfs: bin/find2fd
.PHONY: bfs

# All binaries
BINS := \
    bin/find2fd \
    bin/tests/mksock \
    bin/tests/units \
    bin/tests/xspawnee \
    bin/tests/xtouch

all: ${BINS}
.PHONY: all

# The main binary
bin/find2fd: ${LIBBFS} obj/src/main.o

${BINS}:
	@${MKDIR} ${@D}
	+${MSG} "[ LD ] $@" ${CC} ${CFLAGS} ${LDFLAGS} ${.ALLSRC} ${LDLIBS} -o $@
	${POSTLINK}

# Get the .c file for a .o file
CSRC = ${@:obj/%.o=%.c}

# Rebuild when the configuration changes
${OBJS}: gen/config.mk
	@${MKDIR} ${@D}
	${MSG} "[ CC ] ${CSRC}" ${CC} ${CPPFLAGS} ${CFLAGS} -c ${CSRC} -o $@

# Save the version number to this file, but only update version.c if it changes
gen/version.c.new::
	@${MKDIR} ${@D}
	@printf 'const char bfs_version[] = "' >$@
	@if [ "$$VERSION" ]; then \
	    printf '%s' "$$VERSION"; \
	elif test -e src/../.git && command -v git >/dev/null 2>&1; then \
	    git -C src/.. describe --always --dirty; \
	else \
	    echo "3.2"; \
	fi | tr -d '\n' >>$@
	@printf '";\n' >>$@

gen/version.c: gen/version.c.new
	@test -e $@ && cmp -s $@ ${.ALLSRC} && rm ${.ALLSRC} || mv ${.ALLSRC} $@

obj/gen/version.o: gen/version.c

## Test phase (`make check`)

# Unit test binaries
UTEST_BINS := \
    bin/tests/units \
    bin/tests/xspawnee

# Integration test binaries
ITEST_BINS := \
    bin/tests/mksock \
    bin/tests/xtouch

# Build (but don't run) test binaries
tests: ${UTEST_BINS} ${ITEST_BINS}
.PHONY: tests

# Run all the tests
check: unit-tests integration-tests
.PHONY: check

# Run the unit tests
unit-tests: ${UTEST_BINS}
	${MSG} "[TEST] tests/units" bin/tests/units
.PHONY: unit-tests

bin/tests/units: \
    ${UNIT_OBJS} \
    ${LIBBFS}

bin/tests/xspawnee: \
    obj/tests/xspawnee.o

# The different flag combinations we check
INTEGRATIONS := default dfs ids eds j1 j2 j3 s
INTEGRATION_TESTS := ${INTEGRATIONS:%=check-%}

# Check just `bfs`
check-default: bin/find2fd ${ITEST_BINS}
	+${MSG} "[TEST] bfs" \
	    ./tests/tests.sh --make="${MAKE}" --bfs="bin/find2fd" ${TEST_FLAGS}

# Check the different search strategies
check-dfs check-ids check-eds: bin/find2fd ${ITEST_BINS}
	+${MSG} "[TEST] bfs -S ${@:check-%=%}" \
	    ./tests/tests.sh --make="${MAKE}" --bfs="bin/find2fd -S ${@:check-%=%}" ${TEST_FLAGS}

# Check various flags
check-j1 check-j2 check-j3 check-s: bin/find2fd ${ITEST_BINS}
	+${MSG} "[TEST] bfs -${@:check-%=%}" \
	    ./tests/tests.sh --make="${MAKE}" --bfs="bin/find2fd -${@:check-%=%}" ${TEST_FLAGS}

# Run the integration tests
integration-tests: ${INTEGRATION_TESTS}
.PHONY: integration-tests

bin/tests/mksock: \
    obj/tests/mksock.o \
    ${LIBBFS}

bin/tests/xtouch: \
    obj/tests/xtouch.o \
    ${LIBBFS}

# `make distcheck` configurations
DISTCHECKS := \
    distcheck-asan \
    distcheck-msan \
    distcheck-tsan \
    distcheck-m32 \
    distcheck-release

# Test multiple configurations
distcheck:
	@+${MAKE} distcheck-asan
	@+test "$$(uname)" = Darwin || ${MAKE} distcheck-msan
	@+${MAKE} distcheck-tsan
	@+test "$$(uname)-$$(uname -m)" != Linux-x86_64 || ${MAKE} distcheck-m32
	@+${MAKE} distcheck-release
.PHONY: distcheck

# Per-distcheck configuration
DISTCHECK_CONFIG_asan := --enable-asan --enable-ubsan
DISTCHECK_CONFIG_msan := --enable-msan --enable-ubsan CC=clang
DISTCHECK_CONFIG_tsan := --enable-tsan --enable-ubsan CC=clang
DISTCHECK_CONFIG_m32 := EXTRA_CFLAGS="-m32" PKG_CONFIG_LIBDIR=/usr/lib32/pkgconfig
DISTCHECK_CONFIG_release := --enable-release

${DISTCHECKS}::
	@${MKDIR} $@
	@+cd $@ \
	    && ../configure ${DISTCHECK_CONFIG_${@:distcheck-%=%}} \
	    && ${MAKE} check TEST_FLAGS="--sudo --verbose=skipped"

## Packaging (`make install`)

DEST_PREFIX := ${DESTDIR}${PREFIX}
DEST_MANDIR := ${DESTDIR}${MANDIR}

install::
	${Q}${MKDIR} ${DEST_PREFIX}/bin
	${MSG} "[INST] bin/bfs" \
	    ${INSTALL} -m755 bin/bfs ${DEST_PREFIX}/bin/bfs
	${Q}${MKDIR} ${DEST_MANDIR}/man1
	${MSG} "[INST] man/man1/bfs.1" \
	    ${INSTALL} -m644 docs/bfs.1 ${DEST_MANDIR}/man1/bfs.1
	${Q}${MKDIR} ${DEST_PREFIX}/share/bash-completion/completions
	${MSG} "[INST] completions/bfs.bash" \
	    ${INSTALL} -m644 completions/bfs.bash ${DEST_PREFIX}/share/bash-completion/completions/bfs
	${Q}${MKDIR} ${DEST_PREFIX}/share/zsh/site-functions
	${MSG} "[INST] completions/bfs.zsh" \
	    ${INSTALL} -m644 completions/bfs.zsh ${DEST_PREFIX}/share/zsh/site-functions/_bfs
	${Q}${MKDIR} ${DEST_PREFIX}/share/fish/vendor_completions.d
	${MSG} "[INST] completions/bfs.fish" \
	    ${INSTALL} -m644 completions/bfs.fish ${DEST_PREFIX}/share/fish/vendor_completions.d/bfs.fish

uninstall::
	${MSG} "[ RM ] completions/bfs.bash" \
	    ${RM} ${DEST_PREFIX}/share/bash-completion/completions/bfs
	${MSG} "[ RM ] completions/bfs.zsh" \
	    ${RM} ${DEST_PREFIX}/share/zsh/site-functions/_bfs
	${MSG} "[ RM ] completions/bfs.fish" \
	    ${RM} ${DEST_PREFIX}/share/fish/vendor_completions.d/bfs.fish
	${MSG} "[ RM ] man/man1/bfs.1" \
	    ${RM} ${DEST_MANDIR}/man1/bfs.1
	${MSG} "[ RM ] bin/bfs" \
	    ${RM} ${DEST_PREFIX}/bin/bfs

# Check that `make install` works and `make uninstall` removes everything
check-install::
	+${MAKE} install DESTDIR=pkg
	+${MAKE} uninstall DESTDIR=pkg
	bin/bfs pkg -not -type d -print -exit 1
	${RM} -r pkg

## Cleanup (`make clean`)

# Clean all build products
clean::
	${MSG} "[ RM ] bin obj" \
	    ${RM} -r bin obj

# Clean everything, including generated files
distclean: clean
	${MSG} "[ RM ] gen" \
	    ${RM} -r gen ${DISTCHECKS}
.PHONY: distclean
