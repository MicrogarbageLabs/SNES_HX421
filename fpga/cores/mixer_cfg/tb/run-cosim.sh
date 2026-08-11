#!/bin/sh
# run-cosim.sh — co-simulate hx_mixer_cfg.v (STM32 register -> mixer cfg decoder)
# against the golden model. Icarus Verilog. Prints PASS/FAIL.
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(cd "$(dirname "$0")" && pwd)"

gcc -O2 gen_vectors.c -o gen_vectors.exe
./gen_vectors.exe                         # writes golden.hex

iverilog -g2012 -I.. -o mixer_cfg_sim hx_mixer_cfg_tb.v ../hx_mixer_cfg.v
vvp mixer_cfg_sim | grep -iE "COSIM|write "
