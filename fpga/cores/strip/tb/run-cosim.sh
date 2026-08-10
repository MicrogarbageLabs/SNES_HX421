#!/bin/sh
# run-cosim.sh — co-simulate hx_strip.v against its C reference
# (runtime/hx421_metatile.c). Generates PSRAM + golden vectors from the reference,
# runs the RTL under Icarus Verilog, and diffs 2048 tilemap entries. No license
# needed (iverilog/vvp, not Questa vsim). Prints COSIM PASS/FAIL.
set -e
export PATH=/c/msys64/mingw64/bin:$PATH
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

gcc -O2 -I../../../../runtime gen_vectors.c ../../../../runtime/hx421_metatile.c -o gen_vectors.exe
./gen_vectors.exe

iverilog -g2012 -o strip_sim hx_strip_tb.v ../hx_strip.v
vvp strip_sim | grep -iE "COSIM|DIFF|MISS"
