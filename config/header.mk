# Copyright © Tavian Barnes <tavianator@tavianator.com>
# SPDX-License-Identifier: 0BSD

# Makefile that generates gen/config.h

include config/prelude.mk
include ${GEN}/config.mk
include config/exports.mk

# All header fragments we generate
HEADERS := \
    ${GEN}/getprogname.h \
    ${GEN}/getprogname-gnu.h

${GEN}/config.h: ${HEADERS}
	${MSG} "[ GEN] ${TGT}"
	printf '// %s\n' "${TGT}" >$@
	printf '#ifndef BFS_CONFIG_H\n' >>$@
	printf '#define BFS_CONFIG_H\n' >>$@
	cat ${.ALLSRC} >>$@
	printf '#endif // BFS_CONFIG_H\n' >>$@
	cat ${.ALLSRC:%=%.log} >$@.log
	${RM} ${.ALLSRC} ${.ALLSRC:%=%.log}
	${VCAT} $@
.PHONY: ${GEN}/config.h

# The C source file to attempt to compile
CSRC = ${@:${GEN}/%.h=config/%.c}

${HEADERS}::
	config/cc-define.sh ${CSRC} >$@ 2>$@.log
	if ! [ "${IS_V}" ]; then \
	    if grep -q 'true$$' $@; then \
	        printf '[ CC ] %-${MSG_WIDTH}s  ✔\n' ${CSRC}; \
	    else \
	        printf '[ CC ] %-${MSG_WIDTH}s  ✘\n' ${CSRC}; \
	    fi; \
	fi
