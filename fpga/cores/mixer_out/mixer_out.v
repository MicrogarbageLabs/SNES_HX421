// mixer_out.v — audio final stage (SKELETON, port-level only; body is TODO)
//
// Role (see docs/audio.md): own the SNES-master-locked audio rate and the DAC. The M3 does the actual
// 8-ch mix; this core generates the master/487 tick, hands it to the M3, FIFOs the M3's mixed samples,
// and clocks them out to the DAC at the same tick. Result: ONE interpolation stage (in the M3), zero
// here, and structurally zero A/V drift.
//
// This taps the base's PLL (verilog/sd2snes_mini/pll.v) rather than a fresh oscillator — the whole
// point is that the audio clock derives from the SNES master (cart pin 1 → FPGA).

`default_nettype none

module mixer_out #(
    parameter integer MASTER_DIV   = 487,   // 21.477270 MHz / 487 = 44101 Hz (MSU-1 native)
    parameter integer SAMPLE_BITS  = 16,
    parameter integer FIFO_DEPTH   = 256     // MCU-block runway; size vs. worst-case refill latency
) (
    input  wire                     clk_master,   // SNES master clock domain (from base PLL)
    input  wire                     rst_n,

    // --- Sample-rate tick handed to the MCU (Option B): one block/sample per assert ---
    output wire                     audio_tick,    // master/MASTER_DIV strobe -> MCU IRQ/DMA request

    // --- Mixed stereo samples in from the MCU (via base MCU/SPI bridge), MCU clock domain ---
    input  wire                     s_valid,
    output wire                     s_ready,
    input  wire [SAMPLE_BITS-1:0]   s_left,
    input  wire [SAMPLE_BITS-1:0]   s_right,

    // --- DAC pins (I2S or parallel; pick per board DAC) ---
    output wire                     dac_bclk,
    output wire                     dac_lrck,
    output wire                     dac_data,

    // --- FIFO telemetry for the MCU's produce/consume feedback loop ---
    output wire [$clog2(FIFO_DEPTH):0] fifo_level
);

    // TODO: master/MASTER_DIV divider -> audio_tick  (this is the drift-corrector; keep it exact)
    // TODO: dual-clock FIFO (MCU write side / clk_master read side), depth FIFO_DEPTH
    //       - s_valid/s_ready is the write side
    //       - read side pops one stereo frame per audio_tick
    //       - fifo_level exported so the MCU steers block production (high/low water marks)
    // TODO: DAC serializer clocked from clk_master-derived bit clock (I2S: bclk/lrck/data)
    // TODO: underflow policy — hold last sample (a stall must not shift the sample phase / cause drift)

    // Placeholder tie-offs so the module elaborates; REMOVE as the body lands.
    assign audio_tick = 1'b0;
    assign s_ready    = 1'b0;
    assign dac_bclk   = 1'b0;
    assign dac_lrck   = 1'b0;
    assign dac_data   = 1'b0;
    assign fifo_level = '0;

endmodule

`default_nettype wire
