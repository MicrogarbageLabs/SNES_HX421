`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
//  tb_loop_param — prove the new LOOP_LEN parameter on hx_mixer_dac controls the
//  channel-0 wrap length. With LOOP_LEN=1024 the drain pointer must climb past 127;
//  the old hardwired loop_len=128 could never exceed 127. Mock PSRAM returns a ramp
//  (data == address) so we can also see the mixer fetch high sample indices.
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////
module tb_loop_param;
  reg clk = 0, sysclk = 0;
  always #5.208 clk    = ~clk;      // ~96 MHz
  always #46.6  sysclk = ~sysclk;   // ~10.7 MHz-ish SNES sysclk stand-in

  wire        rom_rd_req;
  wire [23:0] rom_rd_addr;
  reg         rom_rd_ack = 0;
  reg  signed [15:0] rom_rd_data = 0;
  wire [23:0] drain_pos;
  reg  [23:0] max_addr = 0;

  // mock PSRAM: 1-cycle ack, data = low 16 bits of the requested sample index
  always @(posedge clk) begin
    rom_rd_ack <= 1'b0;
    if (rom_rd_req && !rom_rd_ack) begin
      rom_rd_data <= rom_rd_addr[15:0];
      rom_rd_ack  <= 1'b1;
      if (rom_rd_addr > max_addr) max_addr <= rom_rd_addr;
    end
  end

  hx_mixer_dac #(.LOOP_LEN(32'd1024)) dut (
    .clkin(clk), .sysclk(sysclk), .palmode(1'b0), .ext_mute(8'd0),
    .sdout(), .mclk_out(), .lrck_out(),
    .rom_rd_req(rom_rd_req), .rom_rd_addr(rom_rd_addr),
    .rom_rd_ack(rom_rd_ack), .rom_rd_data(rom_rd_data),
    .drain_pos(drain_pos),
    .dbg_tick(), .dbg_mix(), .dbg_sdout(), .dbg_status()
  );

  integer fail = 0;
  initial begin
    #6_000_000;                       // ~6 ms: plenty of 44.1 kHz ticks
    $display("loop-param: LOOP_LEN=1024  drain_pos=%0d  max fetched idx=%0d  cfg_done=%0d",
             drain_pos, max_addr, dut.cfg_done);
    if (dut.cfg_done !== 1'b1) begin $display("FAIL: config FSM never completed"); fail=1; end
    if (drain_pos <= 127)     begin $display("FAIL: drain never passed 127 -> LOOP_LEN not applied (still 128)"); fail=1; end
    if (max_addr  <= 127)     begin $display("FAIL: mixer never fetched a sample index past 127"); fail=1; end
    if (fail) $display("RESULT: FAIL");
    else      $display("RESULT: PASS - LOOP_LEN parameter sets the channel-0 wrap length");
    $finish;
  end
endmodule
