#!/bin/sh
# run-cosim.sh — co-simulate hx_scene.v against the golden model (gen_vectors.c).
# Verifies the emitted 65816 DMA body (register preamble + DMA slots) byte-exact.
# Icarus Verilog (no Questa license). Prints COSIM PASS/FAIL.
set -e
export PATH=/c/msys64/mingw64/bin:$PATH
cd "$(cd "$(dirname "$0")" && pwd)"

gcc -O2 gen_vectors.c -o gen_vectors.exe
./gen_vectors.exe

iverilog -g2012 -o scene_sim hx_scene_tb.v ../hx_scene.v
vvp scene_sim | grep -iE "COSIM|byte |length"
