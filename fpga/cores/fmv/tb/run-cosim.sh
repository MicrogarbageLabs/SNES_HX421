#!/bin/sh
# run-cosim.sh — co-simulate hx_fmv.v against the golden model (gen_vectors.c).
# Verifies the sub-frame PSRAM->BRAM staging copy + the two DMA descriptors
# (CHR w/ double-buffer overlap, tilemap). Icarus Verilog (no Questa license).
set -e
export PATH=/c/msys64/mingw64/bin:$PATH
cd "$(cd "$(dirname "$0")" && pwd)"

gcc -O2 gen_vectors.c -o gen_vectors.exe
./gen_vectors.exe

iverilog -g2012 -o fmv_sim hx_fmv_tb.v ../hx_fmv.v
vvp fmv_sim | grep -iE "CASE|case OK|case FAIL|COSIM"
