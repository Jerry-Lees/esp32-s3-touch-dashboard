#!/bin/sh
# PC test for main/json_skip.c: random JSON (escapes, nesting, names that
# look like dropped ones inside strings) checked against Python. Optionally
# a real file: ./run.sh some.json
set -e
cd "$(dirname "$0")"
gcc -O2 -Wall -Wextra -I ../../../main -o /tmp/json_skip_test test.c ../../../main/json_skip.c
python3 check.py "$@"
