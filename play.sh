#!/bin/sh
# play.sh -- build (if needed) and run the terminal raycaster.
#
#   ./play.sh                    play
#   ./play.sh --demo             watch the autopilot escape the maze
#   ./play.sh --selftest         verify the engine
#
# Works out of the box on any Linux/macOS terminal that has cc/gcc/clang.
set -e
cd "$(dirname "$0")"

if [ ! -x ./raycaster ] || [ raycaster.c -nt raycaster ]; then
    echo "building raycaster..."
    ${CC:-cc} -O2 -std=c99 -Wall -Wextra -pedantic -o raycaster raycaster.c -lm
fi

exec ./raycaster "$@"
