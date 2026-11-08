#!/bin/sh
# Run the recompiled game from the repo root's extracted/ (log: mua3.log). Extra env vars pass through
# (LARECOMP_* debugging switches, see the runtime). Usage: tools/run.sh [seconds to run, default: until closed]
cd "$(dirname "$0")/.." || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
export LARECOMP_DATA_ROOT="$(pwd -W 2>/dev/null || pwd)"
if [ -n "$1" ]; then
    build/mua3recomp.exe extracted > mua3.log 2>&1 &
    pid=$!
    sleep "$1"
    kill "$pid" 2>/dev/null
    taskkill //F //IM mua3recomp.exe > /dev/null 2>&1
    wait "$pid" 2>/dev/null
else
    build/mua3recomp.exe extracted 2>&1 | tee mua3.log
fi
