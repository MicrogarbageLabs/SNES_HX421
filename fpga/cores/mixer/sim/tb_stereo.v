`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
//  tb_stereo — prove STEREO=1 configures an L/R pair for an interleaved ring:
//  both channels active at step 2.0 (each reads every other sample = one lane),
//  ch0 panned hard-left, ch1 hard-right. Paired with per-channel base (BASE1=BASE0+2
//  in main.v) this reconstructs stereo. Checks the config registers directly.
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////
module tb_stereo;
  reg clk = 0, sysclk = 0;
  always #5.208 clk = ~clk;
  always #46.6  sysclk = ~sysclk;

  reg signed [15:0] wave [0:127];
  initial $readmemh("sine128.hex", wave);
  wire        rom_rd_req;
  wire [23:0] rom_rd_addr;
  wire [2:0]  rom_rd_ch;
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

  hx_mixer_dac #(.LOOP_LEN(32'd128), .STEREO(1'b1)) dut (
    .clkin(clk), .sysclk(sysclk), .palmode(1'b0), .ext_mute(8'd0),
    .sdout(), .mclk_out(), .lrck_out(),
    .rom_rd_req(rom_rd_req), .rom_rd_addr(rom_rd_addr), .rom_rd_ch(rom_rd_ch),
    .rom_rd_ack(rom_rd_ack), .rom_rd_data(rom_rd_data),
    .drain_pos(drain_pos),
    .dbg_tick(), .dbg_mix(), .dbg_sdout(), .dbg_status()
  );

  integer fail = 0;
  initial begin
    #3_000_000;
    $display("stereo: active0=%0d active1=%0d step0=%h step1=%h",
             dut.u_mix.active[0], dut.u_mix.active[1], dut.u_mix.step[0], dut.u_mix.step[1]);
    $display("stereo: ch0 pan_l=%h pan_r=%h  ch1 pan_l=%h pan_r=%h",
             dut.u_mix.pan_l[0], dut.u_mix.pan_r[0], dut.u_mix.pan_l[1], dut.u_mix.pan_r[1]);
    if (dut.u_mix.active[0] !== 1'b1 || dut.u_mix.active[1] !== 1'b1) begin $display("FAIL: both channels not active"); fail=1; end
    if (dut.u_mix.step[0] !== 64'h0000_0002_0000_0000) begin $display("FAIL: ch0 step != 2.0"); fail=1; end
    if (dut.u_mix.step[1] !== 64'h0000_0002_0000_0000) begin $display("FAIL: ch1 step != 2.0"); fail=1; end
    if (dut.u_mix.pan_l[0] !== 16'h7FFF || dut.u_mix.pan_r[0] !== 16'h0000) begin $display("FAIL: ch0 not hard-left"); fail=1; end
    if (dut.u_mix.pan_l[1] !== 16'h0000 || dut.u_mix.pan_r[1] !== 16'h7FFF) begin $display("FAIL: ch1 not hard-right"); fail=1; end
    if (fail) $display("RESULT: FAIL");
    else      $display("RESULT: PASS - stereo pair (step 2.0, hard L/R) configured");
    $finish;
  end
endmodule
