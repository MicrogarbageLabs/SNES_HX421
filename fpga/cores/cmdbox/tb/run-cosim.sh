#!/bin/sh
# run-cosim.sh — co-simulate hx_cmdbox.v (SNES->cart command mailbox) against the
# golden model. Drives the SNES+STM32 contract (write block, ring doorbell, read
# back, ack, reuse, doorbell/ack collision). Icarus Verilog. Prints PASS/FAIL.
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(cd "$(dirname "$0")" && pwd)"

gcc -O2 gen_vectors.c -o gen_vectors.exe
./gen_vectors.exe                         # writes golden.hex

iverilog -g2012 -I.. -o cmdbox_sim hx_cmdbox_tb.v ../hx_cmdbox.v
vvp cmdbox_sim | grep -iE "COSIM|check "
