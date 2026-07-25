`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
//  hx_tone_dac — HX-421 audio-seam bring-up (H4a).
//
//  The smallest thing that makes sound on real hardware: a loud square wave fed
//  straight through the proven MSU-1 DAC back half (dac_mix = dac.v with a direct
//  sample input). No SNES config, no PSRAM, no mixer -- just power on and a tone
//  should come out the cart's audio. This isolates the audio SEAM (does our
//  sample source reach the I2S DAC?) from the mixer (does it compute the right
//  samples?, already proven bit-exact in sim).
//
//  DIAGNOSTIC (H4a debug): a sealed cart can't be scoped, so this module also
//  exports live counters + sticky "this stage fired" flags for each seam stage,
//  which main.v serves through the SNES read window. One flash then tells us
//  exactly which stage is dead on hardware:
//     dbg_tick   - sample tick (44.1 kHz comb_strobe) firing -> phase acc + sysclk
//     dbg_tone   - tone_sign toggling                        -> generator advancing
//     dbg_sdout  - sdout edges                               -> I2S serializing data
//     dbg_status - sticky evidence bits + volume-ramped flag
//
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////

module hx_tone_dac(
  input  clkin,       // CLK2, 96 MHz
  input  sysclk,      // SNES_SYSCLK, ~21.477 MHz master (sets the 44.1 kHz rate)
  input  palmode,     // 0 = NTSC (44.1 kHz), 1 = PAL
  output sdout,
  output mclk_out,
  output lrck_out,
  // ---- diagnostic bus (served at the SNES read window; no functional effect) ----
  output [7:0] dbg_tick,    // ++ each 44.1 kHz sample tick
  output [7:0] dbg_tone,    // ++ each tone half-period (square edge)
  output [7:0] dbg_sdout,   // ++ each sdout transition
  output [7:0] dbg_status   // {3'b0, vol_ramped, smp_nz, sdout_seen, tone_seen, tick_seen}
);

  // Power-up reset: hold the DAC (CIC state, phase accumulator) in reset for the
  // first ~1024 clocks, then release. Gives a clean, deterministic start in both
  // simulation (no X's through the CIC) and on silicon.
  reg [9:0] por = 10'd0;
  reg       por_rst = 1'b1;
  always @(posedge clkin) begin
    if (por != 10'h3FF) por <= por + 10'd1;
    else                por_rst <= 1'b0;
  end

  // Square-wave tone. The DAC asks for a new sample once per 44.1 kHz tick via
  // sample_req; toggle sign every HALF ticks -> a square at 44100/(2*HALF) Hz.
  //   HALF = 50 -> 441 Hz (about a concert-A). AMP well under full scale so the
  //   CIC interpolator's overshoot cannot clip.
  localparam [15:0] TONE_HALF = 16'd50;
  localparam [15:0] TONE_AMP  = 16'h3000;   // ~0.375 full scale

  wire        sample_req;
  reg  [15:0] tone_cnt  = 16'd0;
  reg         tone_sign = 1'b0;
  always @(posedge clkin) begin
    if (por_rst) begin
      tone_cnt  <= 16'd0;
      tone_sign <= 1'b0;
    end else if (sample_req) begin
      if (tone_cnt >= TONE_HALF - 16'd1) begin
        tone_cnt  <= 16'd0;
        tone_sign <= ~tone_sign;
      end else begin
        tone_cnt <= tone_cnt + 16'd1;
      end
    end
  end

  wire signed [15:0] tone_samp = tone_sign ? $signed(TONE_AMP)
                                           : -$signed(TONE_AMP);

  wire [10:0]        dac_vol_reg;
  wire signed [15:0] dac_vol_sample;

  // Same sample on both channels (mono tone).
  dac_mix u_dac(
    .clkin(clkin),
    .sysclk(sysclk),
    .volume(8'hFF),
    .vol_latch(1'b1),
    .vol_select(3'b000),
    .dac_address_ext(9'd0),
    .play(1'b1),
    .reset(por_rst),
    .palmode(palmode),
    .sample_in({tone_samp, tone_samp}),
    .sample_req(sample_req),
    .sdout(sdout),
    .mclk_out(mclk_out),
    .lrck_out(lrck_out),
    .sclk_out(),
    .DAC_STATUS(),
    .dbg_vol_reg(dac_vol_reg),
    .dbg_vol_sample(dac_vol_sample)
  );

  // ---- diagnostic counters + sticky evidence flags ----
  reg [7:0] tick_cnt  = 8'd0;
  reg [7:0] tone_ecnt = 8'd0;
  reg [7:0] sdout_cnt = 8'd0;
  reg       tick_seen = 1'b0, tone_seen = 1'b0, sdout_seen = 1'b0;
  reg       smp_nz = 1'b0, vol_ramped = 1'b0;
  reg       sign_d = 1'b0, sdout_d = 1'b0;

  always @(posedge clkin) begin
    sign_d  <= tone_sign;
    sdout_d <= sdout;
    if (sample_req)          begin tick_cnt  <= tick_cnt  + 8'd1; tick_seen  <= 1'b1; end
    if (tone_sign !== sign_d) begin tone_ecnt <= tone_ecnt + 8'd1; tone_seen  <= 1'b1; end
    if (sdout   !== sdout_d)  begin sdout_cnt <= sdout_cnt + 8'd1; sdout_seen <= 1'b1; end
    if (dac_vol_sample != 16'sd0) smp_nz     <= 1'b1;
    if (dac_vol_reg >= 11'd200)   vol_ramped <= 1'b1;
  end

  assign dbg_tick   = tick_cnt;
  assign dbg_tone   = tone_ecnt;
  assign dbg_sdout  = sdout_cnt;
  assign dbg_status = {3'b000, vol_ramped, smp_nz, sdout_seen, tone_seen, tick_seen};

endmodule
