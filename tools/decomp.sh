#!/bin/sh
# usage: tools/decomp.sh <out.txt> <addr>...
cd "$(dirname "$0")/.." && /opt/ghidra/support/analyzeHeadless re/ghidra btw -process dmcr.exe -noanalysis -readOnly \
  -scriptPath re/scripts -postScript Decomp.java "$@" > re/ghidra/decomp.log 2>&1 || tail -20 re/ghidra/decomp.log
