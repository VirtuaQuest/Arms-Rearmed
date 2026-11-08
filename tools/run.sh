#!/bin/sh
# Run the recompiled game from the repo root's extracted/ (log: arms.log). Extra env vars pass through
# (LARECOMP_* debugging switches, see the runtime). Usage: tools/run.sh [seconds to run, default: until closed]
cd "$(dirname "$0")/.." || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
export LARECOMP_DATA_ROOT="$(pwd -W 2>/dev/null || pwd)"
if [ -n "$1" ]; then
    build/armsrecomp.exe extracted > arms.log 2>&1 &
    pid=$!
    sleep "$1"
    kill "$pid" 2>/dev/null
    taskkill //F //IM armsrecomp.exe > /dev/null 2>&1
    wait "$pid" 2>/dev/null
else
    build/armsrecomp.exe extracted 2>&1 | tee arms.log
fi
