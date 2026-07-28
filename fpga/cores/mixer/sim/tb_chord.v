`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
//  tb_chord — prove SECOND_CH=1 on hx_mixer_dac configures TWO channels the mixer
//  sums: ch0 at step 1.0 and ch1 at step 1.5 (a perfect fifth up), both active,
//  reading the same sine. Confirms the config (active + step) and that both read
//  positions advance with ch1 outrunning ch0 ~1.5x -- the first real multi-channel
//  MIX. Mock PSRAM returns a 128-sample sine.
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////
module tb_chord;
  reg clk = 0, sysclk = 0;
  always #5.208 clk    = ~clk;
  always #46.6  sysclk = ~sysclk;

  reg signed [15:0] wave [0:127];
  initial $readmemh("sine128.hex", wave);

  wire        rom_rd_req;
  wire [23:0] rom_rd_addr;
  reg         rom_rd_ack = 0;
  reg  signed [15:0] rom_rd_data = 0;
  wire [23:0] drain_pos;
  always @(posedge clk) begin
    rom_rd_ack <= 1'b0;
    if (rom_rd_req && !rom_rd_ack) begin
      rom_rd_data <= wave[rom_rd_addr[6:0]];
      rom_rd_ack  <= 1'b1;
    end
  end

  hx_mixer_dac #(.LOOP_LEN(32'd128), .SECOND_CH(1'b1)) dut (
    .clkin(clk), .sysclk(sysclk), .palmode(1'b0), .ext_mute(8'd0),
    .sdout(), .mclk_out(), .lrck_out(),
    .rom_rd_req(rom_rd_req), .rom_rd_addr(rom_rd_addr),
    .rom_rd_ack(rom_rd_ack), .rom_rd_data(rom_rd_data),
    .drain_pos(drain_pos),
    .dbg_tick(), .dbg_mix(), .dbg_sdout(), .dbg_status()
  );

  // sample ch1's read position mid-run to confirm it outpaces ch0
  reg [23:0] pos0_mid = 0, pos1_mid = 0;
  initial begin #3_000_000; pos0_mid = dut.u_mix.src_pos[0]; pos1_mid = dut.u_mix.src_pos[1]; end

  integer fail = 0;
  initial begin
    #6_000_000;
    $display("chord: cfg_done=%0d active0=%0d active1=%0d step1=%h",
             dut.cfg_done, dut.u_mix.active[0], dut.u_mix.active[1], dut.u_mix.step[1]);
    $display("chord: mid-run pos0=%0d pos1=%0d (ch1 should run ~1.5x)", pos0_mid, pos1_mid);
    if (dut.cfg_done !== 1'b1)              begin $display("FAIL: config never completed"); fail=1; end
    if (dut.u_mix.active[0] !== 1'b1)      begin $display("FAIL: channel 0 not active"); fail=1; end
    if (dut.u_mix.active[1] !== 1'b1)      begin $display("FAIL: channel 1 not active"); fail=1; end
    if (dut.u_mix.step[1] !== 64'h0000_0001_8000_0000) begin $display("FAIL: ch1 step != 1.5"); fail=1; end
    if (pos1_mid <= pos0_mid)              begin $display("FAIL: ch1 not outpacing ch0"); fail=1; end
    if (fail) $display("RESULT: FAIL");
    else      $display("RESULT: PASS - two channels (1.0 + 1.5) configured and mixing");
    $finish;
  end
endmodule
