# Soft-core bench — measuring candidates on the real part

Purpose: pick the RISC-V core on **measured** LE count and Fmax for `EP4CE15F17C8N`, not on
published figures. Nearly all quoted core sizes are for LUT6 architectures (Xilinx) or faster speed
grades; neither transfers to a Cyclone IV E **C8**. Expect a LUT4 penalty of roughly 1.5–2x on area
and a meaningful derate on Fmax.

## What we are choosing between

| core | HDL | notes |
|---|---|---|
| **VexRiscv** | Verilog (generated from SpinalHDL) | configurable caches — the knob that matters for XIP from PSRAM |
| **Ibex** | SystemVerilog | plain HDL, 2-stage, heavily verified, easy to read |
| **NEORV32** | VHDL | complete SoC, modest IPC |
| (PicoRV32) | Verilog | high Fmax, poor IPC — useful as a floor reference |

Target budget (see `docs/hardware-budget.md`): **~7K LE for the CPU**, after reserving ~3K for the
sd2snes base, ~4K for a later TBDR renderer, and ~1.5K for cache/arbiter/DMA/SPI support.

## Method

Synthesize each core **standalone** — no sd2snes base — so the numbers isolate the core.

1. Copy `template.qsf` to `<core>/<core>.qsf`, set `TOP_LEVEL_ENTITY` and add the core's sources.
2. Write a thin wrapper that **registers every I/O**. This is the step people skip and it invalidates
   the result: without it the fitter reports pad-to-pad paths and you measure the pins, not the core.
   ```verilog
   module bench_top (input clk, input rst_n, input [7:0] din, output reg [7:0] dout);
     reg [7:0] din_r;  always @(posedge clk) din_r <= din;      // register in
     wire [7:0] core_out;
     the_core u_core (.clk(clk), .rst_n(rst_n), /* ... */);
     always @(posedge clk) dout <= core_out;                    // register out
   endmodule
   ```
3. Constrain the clock in an `.sdc` at the target and read the slack:
   ```tcl
   create_clock -name {clk} -period 25.000 [get_ports {clk}]   # 40 MHz
   # then retry at 12.500 (80 MHz) to find the ceiling
   ```
4. Build and read two numbers out of the reports:
   - **Total logic elements** — Fitter report, "Resource Usage Summary"
   - **Fmax** — TimeQuest "Slow 1200mV 85C Model" -> Fmax Summary. Use the **restricted** Fmax;
     the unrestricted figure ignores minimum pulse-width limits and flatters the result.
5. Record both in the table below.

Run each core at 40 MHz first. A core that cannot close 40 MHz standalone will not close it once the
sd2snes base, the PSRAM arbiter and the staging DMA are competing for routing.

## Interpreting

- **Utilisation vs timing.** On a C8 part, congestion degrades Fmax sharply above ~70% utilisation.
  A core that fits in 6K LE but leaves the design at 75% may close *worse* than a 4K core at 55%.
  Judge the candidates on Fmax at the whole-design utilisation you expect, not standalone.
- **IPC is not measured here.** Synthesis gives area and clock only. VexRiscv's advantage over
  PicoRV32 is IPC (~0.7-0.9 vs ~0.2), which no fitter report will show. If two cores land close on
  area and Fmax, prefer the deeper pipeline.
- **Hardware multiply is close to free.** EP4CE15 has **56 idle 18x18 multipliers**. Configure RV32**M**
  onto them rather than LE-based logic: software multiply costs 30-40 cycles and both the game's
  fixed-point math and the TBDR's vertex transforms hit it constantly.

## Results

Fill in as measured. Leave blank rather than estimating.

| core | config | LEs | % of 15408 | DSP | Fmax @C8 (restricted) | closes 40? | closes 80? |
|---|---|---|---|---|---|---|---|
| VexRiscv | min, RV32IM | | | | | | |
| VexRiscv | + I$ 8K | | | | | | |
| VexRiscv | + I$ 16K + D$ | | | | | | |
| Ibex | small, RV32IM | | | | | | |
| PicoRV32 | RV32IM (floor ref) | | | | | | |

## Also worth capturing while Quartus is open

From the **unmodified sd2snes_mini** build — the three numbers the whole hardware budget is sized
around and which are currently guesses:

- **M9K blocks the base consumes** (we assumed ~5 of 56)
- **LEs the base consumes** (we assumed ~2.5-3.5K of 15408)
- SRAM the stock firmware leaves free (needs the ARM half — from the `.map`, via Docker)
