`timescale 1ns / 1ps
// ============================================================
//  tb_tone_level.v — measure the ACTUAL output amplitude of the tone through
//  the real dac_mix CIC, in sim. dac_mix is the identical RTL that runs on
//  hardware, so the peak sample magnitude here IS what the FXPak's DAC receives.
//
//  The seam is proven live on hardware (counters move, sample nonzero, full
//  volume) yet silent, and there is no downstream mute -- so the open question
//  is whether the CIC output for a held-constant square is a healthy amplitude
//  or near-zero. This peeks vol_sample_sat (the value clocked into the I2S
//  shifter) and dac_data_ch (raw CIC output) and reports their peak |value|.
//
//    peak ~ +/-12288 (0x3000)  -> FPGA drives a LOUD waveform; look elsewhere.
//    peak tiny                 -> the CIC mangles the square; that's the bug.
// ============================================================
`default_nettype none

module tb_tone_level;
  reg clkin = 1'b0, sysclk = 1'b0;
  wire sdout, mclk_out, lrck_out;
  wire [7:0] dbg_tick, dbg_tone, dbg_sdout, dbg_status;

  hx_tone_dac dut(
    .clkin(clkin), .sysclk(sysclk), .palmode(1'b0),
    .sdout(sdout), .mclk_out(mclk_out), .lrck_out(lrck_out),
    .dbg_tick(dbg_tick), .dbg_tone(dbg_tone), .dbg_sdout(dbg_sdout), .dbg_status(dbg_status)
  );

  always #5.2083 clkin = ~clkin;
  always #23.279 sysclk = ~sysclk;

  integer peak_out = 0, peak_cic = 0;
  integer v, c, started = 0;
  real t0;

  always @(posedge clkin) begin
    // let volume ramp + CIC settle for 5 ms before measuring
    if ($realtime > 5_000_000.0) begin
      v = dut.u_dac.dbg_vol_sample;              // value into the I2S shifter (signed 16)
      if (v > 32767) v = v - 65536;              // interpret as signed
      if (v < 0) v = -v;
      if (v > peak_out) peak_out = v;

      c = dut.u_dac.dac_data_ch;                 // raw CIC output tap (signed 16)
      if (c > 32767) c = c - 65536;
      if (c < 0) c = -c;
      if (c > peak_cic) peak_cic = c;
    end
  end

  initial begin
    #35_000_000;   // 35 ms
    $display("tone-level: input square amplitude = %0d (0x3000)", 16'h3000);
    $display("tone-level: peak |CIC output tap|      = %0d", peak_cic);
    $display("tone-level: peak |sample to I2S shift| = %0d  (full scale 32767)", peak_out);
    if (peak_out < 1000)
      $display("RESULT: SUSPECT - output amplitude is tiny; the CIC is killing the square");
    else if (peak_out > 6000)
      $display("RESULT: LOUD - FPGA drives a healthy-amplitude waveform; fault is elsewhere");
    else
      $display("RESULT: MODEST - audible but low; worth boosting");
    $finish;
  end
endmodule

`default_nettype wire
