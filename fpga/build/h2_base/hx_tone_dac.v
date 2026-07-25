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
//  Once a tone is heard, the same seam takes hx_audio_top's output instead of the
//  square generator -- that is H4b.
//
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////

module hx_tone_dac(
  input  clkin,       // CLK2, 96 MHz
  input  sysclk,      // SNES_SYSCLK, ~21.477 MHz master (sets the 44.1 kHz rate)
  input  palmode,     // 0 = NTSC (44.1 kHz), 1 = PAL
  output sdout,
  output mclk_out,
  output lrck_out
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
    .DAC_STATUS()
  );

endmodule
