#!/bin/sh
# run-cosim.sh — co-simulate hx_actor.v against the golden model (gen_vectors.c).
# The golden is an independent straight-line C model of the intended algorithm
# (Y-band sort + priority-lock + rotating flicker window); a match catches
# transcription/FSM bugs. Icarus Verilog (no Questa license). Prints COSIM PASS/FAIL.
set -e
export PATH=/c/msys64/mingw64/bin:$PATH
cd "$(cd "$(dirname "$0")" && pwd)"

gcc -O2 gen_vectors.c -o gen_vectors.exe
./gen_vectors.exe

iverilog -g2012 -o actor_sim hx_actor_tb.v ../hx_actor.v
vvp actor_sim | grep -iE "CASE|COSIM|case OK|FAIL|slot"
