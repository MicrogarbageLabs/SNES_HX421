`timescale 1ns / 1ps
// ============================================================
//  tb_tone_dac.v — prove the HX-421 audio SEAM in simulation before any
//  hardware compile. Instantiates the exact hx_tone_dac (square generator +
//  dac_mix = the proven MSU-1 DAC back half with a direct sample source) that
//  main.v drops in, drives it with real-rate clocks, and checks:
//
//    1. sample_req (the 44.1 kHz tick) actually fires standalone (no MSU) at the
//       right rate  -> the phase accumulator runs once por_rst releases.
//    2. lrck_out toggles                                    -> I2S frame clock alive.
//    3. sdout carries data (many transitions) once the tone is nonzero, and the
//       tone sign toggles at ~441 Hz                        -> our sample reaches I2S.
//
//  A tone through a sealed cart is un-scopable, so this is the only place the
//  seam can be verified without John's ears. Bit-exact audio VALUES are already
//  proven for the mixer; here we only prove the plumbing carries a signal.
// ============================================================
`default_nettype none

module tb_tone_dac;
  reg clkin = 1'b0;    // 96 MHz
  reg sysclk = 1'b0;   // ~21.477 MHz SNES master

  wire sdout, mclk_out, lrck_out;

  hx_tone_dac dut(
    .clkin(clkin), .sysclk(sysclk), .palmode(1'b0),
    .sdout(sdout), .mclk_out(mclk_out), .lrck_out(lrck_out)
  );

  // 96 MHz  -> half period 5.2083 ns
  always #5.2083 clkin = ~clkin;
  // 21.477 MHz -> half period 23.279 ns
  always #23.279 sysclk = ~sysclk;

  // ---- observers (hierarchical peek at the internal tick / tone) ----
  integer req_count = 0;
  real    last_req_t = 0.0, sum_req_dt = 0.0; integer req_dt_n = 0;
  reg     req_d = 1'b0;
  always @(posedge clkin) begin
    req_d <= dut.sample_req;
    if (dut.sample_req && !req_d) begin
      req_count = req_count + 1;
      if (req_count > 1) begin
        sum_req_dt = sum_req_dt + ($realtime - last_req_t);
        req_dt_n = req_dt_n + 1;
      end
      last_req_t = $realtime;
    end
  end

  // tone sign toggles -> measure tone half-period
  integer tog_count = 0;
  real    last_tog_t = 0.0, sum_tog_dt = 0.0; integer tog_dt_n = 0;
  reg     sign_d = 1'b0;
  always @(posedge clkin) begin
    sign_d <= dut.tone_sign;
    if (dut.tone_sign !== sign_d) begin
      tog_count = tog_count + 1;
      if (tog_count > 1) begin
        sum_tog_dt = sum_tog_dt + ($realtime - last_tog_t);
        tog_dt_n = tog_dt_n + 1;
      end
      last_tog_t = $realtime;
    end
  end

  // sdout activity + lrck activity
  integer sdout_edges = 0, lrck_edges = 0;
  reg sdout_d = 1'b0, lrck_d = 1'b0;
  always @(posedge clkin) begin
    sdout_d <= sdout; lrck_d <= lrck_out;
    if (sdout !== sdout_d) sdout_edges = sdout_edges + 1;
    if (lrck_out !== lrck_d) lrck_edges = lrck_edges + 1;
  end

  real req_hz, tone_hz;
  integer fail = 0;
  initial begin
    // Run ~5 ms: enough for a couple of full 441 Hz tone periods and hundreds of
    // 44.1 kHz sample ticks.
    #5_000_000;

    req_hz  = (req_dt_n  > 0) ? (1.0e9 / (sum_req_dt / req_dt_n))  : 0.0; // dt in ns
    tone_hz = (tog_dt_n  > 0) ? (0.5e9 / (sum_tog_dt / tog_dt_n))  : 0.0; // half-period

    $display("tone-dac seam: sample_req ticks=%0d  rate=%.1f Hz (want ~44100)",
             req_count, req_hz);
    $display("tone-dac seam: tone sign toggles=%0d  tone=%.1f Hz (want ~441)",
             tog_count, tone_hz);
    $display("tone-dac seam: sdout edges=%0d  lrck edges=%0d", sdout_edges, lrck_edges);

    if (req_count < 100)                       begin $display("FAIL: sample_req not ticking"); fail=1; end
    if (req_hz < 43000.0 || req_hz > 45200.0)  begin $display("FAIL: sample rate off"); fail=1; end
    if (tog_count < 3)                         begin $display("FAIL: tone not toggling"); fail=1; end
    if (tone_hz < 420.0 || tone_hz > 462.0)    begin $display("FAIL: tone freq off"); fail=1; end
    if (lrck_edges < 100)                      begin $display("FAIL: I2S LRCK not running"); fail=1; end
    if (sdout_edges < 50)                      begin $display("FAIL: no I2S data on sdout"); fail=1; end

    if (fail) $display("RESULT: FAIL - audio seam broken in sim");
    else      $display("RESULT: PASS - tone reaches I2S DAC output (seam good)");
    $finish;
  end
endmodule

`default_nettype wire
