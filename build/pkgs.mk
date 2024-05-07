# Copyright © Tavian Barnes <tavianator@tavianator.com>
# SPDX-License-Identifier: 0BSD

# Makefile that generates gen/pkgs.mk

include build/prelude.mk
include gen/vars.mk
include gen/flags.mk
include build/exports.mk

HEADERS := ${ALL_PKGS:%=gen/use/%.h}

gen/pkgs.mk: ${HEADERS}
	${MSG} "[ GEN] $@"
	@printf '# %s\n' "$@" >$@
	@gen() { \
	    printf 'PKGS := %s\n' "$$*"; \
	    printf 'CFLAGS += %s\n' "$$(build/pkgconf.sh --cflags "$$@")"; \
	    printf 'LDFLAGS += %s\n' "$$(build/pkgconf.sh --ldflags "$$@")"; \
	    printf 'LDLIBS := %s $${LDLIBS}\n' "$$(build/pkgconf.sh --ldlibs "$$@")"; \
	}; \
	gen $$(grep -l ' true$$' ${.ALLSRC} | sed 's|.*/\(.*\)\.h|\1|') >>$@
	${VCAT} $@

.PHONY: gen/pkgs.mk

# Convert gen/use/foo.h to foo
PKG = ${@:gen/use/%.h=%}

${HEADERS}::
	@${MKDIR} ${@D}
	@build/define-if.sh use/${PKG} build/pkgconf.sh ${PKG} >$@ 2>$@.log; \
	    build/msg-if.sh "[ CC ] use/${PKG}.c" test $$? -eq 0;
