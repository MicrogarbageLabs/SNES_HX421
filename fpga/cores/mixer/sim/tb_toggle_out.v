`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
//  tb_toggle_out — verify TOGGLE+STEREO actually mutes AND pans. Force each toggle
//  phase and check the L/R outputs: phase1 -> only ch0 on L (ch1 muted), phase2 ->
//  only ch1 on R (ch0 muted), phase0 -> both. Per-channel mock (ch0=A, ch1=B).
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////
module tb_toggle_out;
  reg clk = 0, sysclk = 0;
  always #5.208 clk = ~clk;
  always #46.6  sysclk = ~sysclk;

  wire        rom_rd_req;
  wire [23:0] rom_rd_addr;
  wire [2:0]  rom_rd_ch;
  reg         rom_rd_ack = 0;
  reg  signed [15:0] rom_rd_data = 0;
  wire [23:0] drain_pos;
  localparam signed [15:0] A = 16'sd16384, B = 16'sd8192;
  always @(posedge clk) begin
    rom_rd_ack <= 1'b0;
    if (rom_rd_req && !rom_rd_ack) begin
      rom_rd_data <= (rom_rd_ch == 3'd1) ? B : A;
      rom_rd_ack  <= 1'b1;
    end
  end

  hx_mixer_dac #(.LOOP_LEN(32'd128), .STEREO(1'b1), .TOGGLE(1'b1)) dut (
    .clkin(clk), .sysclk(sysclk), .palmode(1'b0),
    .sdout(), .mclk_out(), .lrck_out(),
    .rom_rd_req(rom_rd_req), .rom_rd_addr(rom_rd_addr), .rom_rd_ch(rom_rd_ch),
    .rom_rd_ack(rom_rd_ack), .rom_rd_data(rom_rd_data),
    .drain_pos(drain_pos),
    .dbg_tick(), .dbg_mix(), .dbg_sdout(), .dbg_status()
  );

  integer fail = 0;
  task chk(input [1:0] ph, input want_l, input want_r);
    begin
      force dut.tog_phase = ph;
      #400000;
      $display("phase %0d: out_l=%0d out_r=%0d (want L%0d R%0d)",
               ph, dut.u_mix.out_l, dut.u_mix.out_r, want_l, want_r);
      if (want_l && dut.u_mix.out_l === 0) begin $display("  FAIL: L expected nonzero"); fail=1; end
      if (!want_l && dut.u_mix.out_l !== 0) begin $display("  FAIL: L expected 0 (ch0 muted)"); fail=1; end
      if (want_r && dut.u_mix.out_r === 0) begin $display("  FAIL: R expected nonzero"); fail=1; end
      if (!want_r && dut.u_mix.out_r !== 0) begin $display("  FAIL: R expected 0 (ch1 muted)"); fail=1; end
    end
  endtask

  initial begin
    #1_500_000;               // let config + prime finish
    chk(2'd0, 1, 1);          // both play
    chk(2'd1, 1, 0);          // ch1 muted -> only L
    chk(2'd2, 0, 1);          // ch0 muted -> only R
    if (fail) $display("RESULT: FAIL");
    else      $display("RESULT: PASS - TOGGLE mutes the right channel + pan keeps L/R separate");
    $finish;
  end
endmodule
