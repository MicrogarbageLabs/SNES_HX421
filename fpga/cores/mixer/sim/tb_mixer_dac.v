`timescale 1ns / 1ps
// ============================================================
//  tb_mixer_dac.v — H4b: prove the real mixer drives a sine through dac_mix,
//  in sim, before any hardware compile. hx_mixer_dac is the exact top that
//  main.v drops in. Checks the config FSM completes, the mixer produces frames,
//  and the sample handed to the DAC is a full-amplitude sine (~+/-20480), i.e.
//  the actual 8-channel engine is generating audio -- not a hand-made square.
// ============================================================
`default_nettype none

module tb_mixer_dac;
  reg clkin = 1'b0, sysclk = 1'b0;
  wire sdout, mclk_out, lrck_out;
  wire [7:0] dbg_tick, dbg_mix, dbg_sdout, dbg_status;

  hx_mixer_dac dut(
    .clkin(clkin), .sysclk(sysclk), .palmode(1'b0),
    .sdout(sdout), .mclk_out(mclk_out), .lrck_out(lrck_out),
    .dbg_tick(dbg_tick), .dbg_mix(dbg_mix), .dbg_sdout(dbg_sdout), .dbg_status(dbg_status)
  );

  always #5.2083 clkin = ~clkin;
  always #23.279 sysclk = ~sysclk;

  integer peak = 0, trough = 0, v, mixframes = 0;
  integer zc = 0; reg prev_sign = 0; reg cur_sign;
  real first_zc = 0.0, last_zc = 0.0;

  always @(posedge clkin) begin
    if (dut.mix_valid) mixframes = mixframes + 1;
    // measure the held DAC input after config settles (>2 ms)
    if ($realtime > 2_000_000.0) begin
      v = dut.mix_l_r;
      if (v > 32767) v = v - 65536;     // signed
      if (v > peak)   peak   = v;
      if (v < trough) trough = v;
      // zero-crossing rate -> frequency
      cur_sign = (v >= 0);
      if (cur_sign && !prev_sign) begin  // rising zero crossing
        zc = zc + 1;
        if (zc == 1) first_zc = $realtime;
        last_zc = $realtime;
      end
      prev_sign = cur_sign;
    end
  end

  real freq;
  integer fail = 0;
  initial begin
    #12_000_000;   // 12 ms
    freq = (zc > 1) ? ((zc-1) * 1.0e9 / (last_zc - first_zc)) : 0.0;
    $display("mixer-dac: cfg_done=%0d  mix frames=%0d  status=%02x", dut.cfg_done, mixframes, dbg_status);
    $display("mixer-dac: DAC input peak=%0d trough=%0d (sine amp 20480)", peak, trough);
    $display("mixer-dac: rising zero-crossings=%0d  freq=%.1f Hz (want ~344.5)", zc, freq);

    if (dut.cfg_done !== 1'b1)        begin $display("FAIL: config FSM never completed"); fail=1; end
    if (mixframes < 100)             begin $display("FAIL: mixer not producing frames"); fail=1; end
    if (peak   < 15000)              begin $display("FAIL: output not reaching sine peak"); fail=1; end
    if (trough > -15000)             begin $display("FAIL: output not reaching sine trough"); fail=1; end
    if (freq < 320.0 || freq > 370.0) begin $display("FAIL: sine frequency off"); fail=1; end
    if (dbg_status[4:0] !== 5'b11111) begin $display("FAIL: a diagnostic evidence bit is clear"); fail=1; end

    if (fail) $display("RESULT: FAIL - mixer->DAC path broken in sim");
    else      $display("RESULT: PASS - real mixer drives a full-amplitude sine to the DAC");
    $finish;
  end
endmodule

`default_nettype wire
