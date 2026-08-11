#!/bin/sh
# run-cosim-snes.sh — co-simulate hx_cmdbox_snes.v (SNES-bus mailbox wrapper)
# against the golden model. This is the logic main.v instantiates; verifying it
# here covers the integration decode, since main.v itself can't be simulated
# (Altera megafunctions). Icarus Verilog. Prints PASS/FAIL.
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(cd "$(dirname "$0")" && pwd)"

gcc -O2 gen_vectors_snes.c -o gen_vectors_snes.exe
./gen_vectors_snes.exe                     # writes golden.hex

iverilog -g2012 -I.. -o cmdbox_snes_sim hx_cmdbox_snes_tb.v ../hx_cmdbox_snes.v ../hx_cmdbox.v
vvp cmdbox_snes_sim | grep -iE "COSIM|check "
