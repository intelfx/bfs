#!/bin/sh

# Copyright © Tavian Barnes <tavianator@tavianator.com>
# SPDX-License-Identifier: 0BSD

# Print a success/failure indicator from a makefile:
#
#     $ ./configure
#     [ CC ] use/liburing.c                  ✘
#     [ CC ] use/oniguruma.c                 ✔

set -eu

MSG="$1"
shift

if "$@"; then
    build/msg.sh "$(printf '%-37s  ✔' "$MSG")"
else
    build/msg.sh "$(printf '%-37s  ✘' "$MSG")"
fi
