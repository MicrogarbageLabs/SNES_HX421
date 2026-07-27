`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
//  tb_stereo_out — prove the STEREO config actually SEPARATES output: feed ch0 a
//  different constant than ch1 (per-channel mock PSRAM) and confirm out_l tracks ch0
//  only and out_r tracks ch1 only. tb_stereo checked the pan *config*; this checks
//  the pan *effect* on the mixer's L/R outputs.
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////
module tb_stereo_out;
  reg clk = 0, sysclk = 0;
  always #5.208 clk = ~clk;
  always #46.6  sysclk = ~sysclk;

  wire        rom_rd_req;
  wire [23:0] rom_rd_addr;
  wire [2:0]  rom_rd_ch;
  reg         rom_rd_ack = 0;
  reg  signed [15:0] rom_rd_data = 0;
  wire [23:0] drain_pos;
  localparam signed [15:0] A = 16'sd16384;   // ch0 (L) source constant
  localparam signed [15:0] B = 16'sd4096;    // ch1 (R) source constant
  always @(posedge clk) begin
    rom_rd_ack <= 1'b0;
    if (rom_rd_req && !rom_rd_ack) begin
      rom_rd_data <= (rom_rd_ch == 3'd1) ? B : A;   // ch1 -> B, else A
      rom_rd_ack  <= 1'b1;
    end
  end

  hx_mixer_dac #(.LOOP_LEN(32'd128), .STEREO(1'b1)) dut (
    .clkin(clk), .sysclk(sysclk), .palmode(1'b0),
    .sdout(), .mclk_out(), .lrck_out(),
    .rom_rd_req(rom_rd_req), .rom_rd_addr(rom_rd_addr), .rom_rd_ch(rom_rd_ch),
    .rom_rd_ack(rom_rd_ack), .rom_rd_data(rom_rd_data),
    .drain_pos(drain_pos),
    .dbg_tick(), .dbg_mix(), .dbg_sdout(), .dbg_status()
  );

  integer fail = 0;
  initial begin
    #3_000_000;
    $display("stereo-out: out_l=%0d out_r=%0d (ch0 const=%0d ch1 const=%0d, half vol)",
             dut.u_mix.out_l, dut.u_mix.out_r, A, B);
    // out_l should track A (ch0), out_r should track B (ch1); they must differ and
    // each must have the same SIGN/relative magnitude as its own source.
    if (dut.u_mix.out_l === dut.u_mix.out_r) begin
      $display("FAIL: out_l == out_r -> NOT separated (mono collapse in the mixer)"); fail=1;
    end
    if (dut.u_mix.out_l <= dut.u_mix.out_r) begin
      $display("FAIL: out_l (ch0=%0d) not > out_r (ch1=%0d) as the sources are", A, B); fail=1;
    end
    if (fail) $display("RESULT: FAIL");
    else      $display("RESULT: PASS - mixer separates L(ch0) from R(ch1)");
    $finish;
  end
endmodule
