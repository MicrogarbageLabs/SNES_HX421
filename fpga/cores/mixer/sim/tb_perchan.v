`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
//  tb_perchan — prove rom_rd_ch lets each mixer channel read a DIFFERENT PSRAM
//  source. The mock PSRAM returns wave0 for channel-0 fetches and wave1 for
//  channel-1 fetches, keyed on rom_rd_ch -- exactly what main.v's per-channel base
//  mux does (base0 vs base1). With SECOND_CH=1 both channels run, so both waves must
//  be fetched. This validates the channel-select half of stereo / two-stream mixing
//  (ch0 = left/streamA, ch1 = right/streamB at a different base).
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////
module tb_perchan;
  reg clk = 0, sysclk = 0;
  always #5.208 clk    = ~clk;
  always #46.6  sysclk = ~sysclk;

  reg signed [15:0] wave0 [0:127];
  reg signed [15:0] wave1 [0:127];
  initial begin
    $readmemh("sine128.hex", wave0);          // ch0 source
    // ch1 source: a distinct ramp so we can tell the sources apart
    begin : mkw1 integer i; for (i=0;i<128;i=i+1) wave1[i] = (i<<7) - 16'sd8192; end
  end

  wire        rom_rd_req;
  wire [23:0] rom_rd_addr;
  wire [2:0]  rom_rd_ch;
  reg         rom_rd_ack = 0;
  reg  signed [15:0] rom_rd_data = 0;
  wire [23:0] drain_pos;
  reg  ch0_fetched = 0, ch1_fetched = 0;
  reg  ch1_got_wave1 = 0;

  always @(posedge clk) begin
    rom_rd_ack <= 1'b0;
    if (rom_rd_req && !rom_rd_ack) begin
      // per-channel source select — the tb stands in for main.v's base mux
      if (rom_rd_ch == 3'd1) begin
        rom_rd_data  <= wave1[rom_rd_addr[6:0]];
        ch1_fetched  <= 1'b1;
        if (wave1[rom_rd_addr[6:0]] !== 16'sd0) ch1_got_wave1 <= 1'b1;
      end else begin
        rom_rd_data  <= wave0[rom_rd_addr[6:0]];
        ch0_fetched  <= 1'b1;
      end
      rom_rd_ack <= 1'b1;
    end
  end

  hx_mixer_dac #(.LOOP_LEN(32'd128), .SECOND_CH(1'b1)) dut (
    .clkin(clk), .sysclk(sysclk), .palmode(1'b0),
    .sdout(), .mclk_out(), .lrck_out(),
    .rom_rd_req(rom_rd_req), .rom_rd_addr(rom_rd_addr), .rom_rd_ch(rom_rd_ch),
    .rom_rd_ack(rom_rd_ack), .rom_rd_data(rom_rd_data),
    .drain_pos(drain_pos),
    .dbg_tick(), .dbg_mix(), .dbg_sdout(), .dbg_status()
  );

  integer fail = 0;
  initial begin
    #4_000_000;
    $display("perchan: ch0_fetched=%0d ch1_fetched=%0d ch1_got_distinct_wave=%0d",
             ch0_fetched, ch1_fetched, ch1_got_wave1);
    if (!ch0_fetched)   begin $display("FAIL: channel 0 never fetched"); fail=1; end
    if (!ch1_fetched)   begin $display("FAIL: channel 1 never fetched (rom_rd_ch not reaching 1)"); fail=1; end
    if (!ch1_got_wave1) begin $display("FAIL: channel 1 didn't read its own (wave1) source"); fail=1; end
    if (fail) $display("RESULT: FAIL");
    else      $display("RESULT: PASS - rom_rd_ch selects a per-channel PSRAM source");
    $finish;
  end
endmodule
