#!/bin/sh
# run-cosim.sh — co-simulate hx_dsp.v (mailbox math+trig coprocessor) against the
# golden model. Also (re)generates ../sintab.vh (the sine ROM the RTL includes).
# Drives the 65816 contract (byte-write operands, START, wait, indexed readback)
# for MUL/MAC/DIV/SIN/COS + edge cases. Icarus Verilog. Prints PASS/FAIL.
set -e
export PATH=/c/msys64/mingw64/bin:$PATH
cd "$(cd "$(dirname "$0")" && pwd)"

gcc -O2 gen_vectors.c -lm -o gen_vectors.exe
./gen_vectors.exe                         # writes ../sintab.vh + golden.hex

iverilog -g2012 -I.. -o dsp_sim hx_dsp_tb.v ../hx_dsp.v
vvp dsp_sim | grep -iE "COSIM|op "
