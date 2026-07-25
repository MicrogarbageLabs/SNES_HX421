`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
//  hx_mixer_dac — HX-421 audio-seam H4b: the REAL 8-channel mixer to the DAC.
//
//  Replaces the H4a square generator with hx_mixer_seq (the timing-closed,
//  bit-exact mixer). One channel is configured at boot to loop-play a 128-sample
//  sine baked into the bitstream; the mixer's output feeds dac_mix -> I2S DAC.
//  So on hardware you hear the actual mixer engine, not a hand-made square.
//
//  Sync: the mixer render is driven by dac_mix's own 44.1 kHz sample tick
//  (sample_req / comb_strobe), so exactly one render happens per DAC sample --
//  no two-clock drift. The finished frame is latched and held as the DAC input;
//  dac_mix consumes it on the next tick (one-sample latency, same as H4a).
//
//  A read-port adapter answers the mixer's sample fetches from the sine BRAM
//  with one-cycle latency (the mixer is proven latency-tolerant). The wavetable
//  loops via loop_len=128, step=1.0 -> 44100/128 = 344.5 Hz sine.
//
//  Still requires the SNES ROM to APU-unmute (h4_tone_apu.s) or it's silent.
//  Public domain (CC0). No warranty.
//////////////////////////////////////////////////////////////////////////////////

module hx_mixer_dac(
  input  clkin,       // CLK2, 96 MHz
  input  sysclk,      // SNES_SYSCLK
  input  palmode,
  output sdout,
  output mclk_out,
  output lrck_out,
  // diagnostic bus (served at the SNES read window)
  output [7:0] dbg_tick,    // ++ each DAC sample tick
  output [7:0] dbg_mix,     // ++ each mixer frame produced (out_valid)
  output [7:0] dbg_sdout,   // ++ each sdout transition
  output [7:0] dbg_status   // {2'b0, cfg_done, smp_nz, sdout_seen, mix_seen, tick_seen}
);

  // ---- power-up reset ----
  reg [9:0] por = 10'd0;
  reg       por_rst = 1'b1;
  always @(posedge clkin) begin
    if (por != 10'h3FF) por <= por + 10'd1;
    else                por_rst <= 1'b0;
  end

  // ---- sine wavetable (128 x signed 16), baked into the bitstream ----
  reg signed [15:0] wave [0:127];
  initial $readmemh("sine128.hex", wave);

  // ---- DAC ----
  wire        sample_req;
  reg  signed [15:0] mix_l_r, mix_r_r;      // latest finished mixer frame (held)
  wire [10:0] dbg_vol_reg;
  wire signed [15:0] dbg_vol_sample;
  dac_mix u_dac(
    .clkin(clkin), .sysclk(sysclk),
    .volume(8'hFF), .vol_latch(1'b1), .vol_select(3'b000),
    .dac_address_ext(9'd0), .play(1'b1), .reset(por_rst), .palmode(palmode),
    .sample_in({mix_l_r, mix_r_r}), .sample_req(sample_req),
    .sdout(sdout), .mclk_out(mclk_out), .lrck_out(lrck_out),
    .sclk_out(), .DAC_STATUS(),
    .dbg_vol_reg(dbg_vol_reg), .dbg_vol_sample(dbg_vol_sample)
  );

  // ---- mixer read port -> sine BRAM (one-cycle latency) ----
  wire        rd_req;
  wire [2:0]  rd_ch;
  wire [31:0] rd_addr;
  reg         rd_ack;
  reg  signed [15:0] rd_data;
  always @(posedge clkin) begin
    rd_ack <= 1'b0;
    if (por_rst) begin
      rd_ack <= 1'b0;
    end else if (rd_req && !rd_ack) begin
      rd_data <= wave[rd_addr[6:0]];
      rd_ack  <= 1'b1;
    end
  end

  // mixer outputs (declared before use in the FSM below)
  wire signed [31:0] mix_l, mix_r;
  wire        mix_valid, mix_busy;

  // ---- config / control FSM: set up channel 0, prime, then render per tick ----
  reg        cfg_we;
  reg [2:0]  cfg_field;
  reg [31:0] cfg_data;
  reg        mix_prime, mix_render, cfg_done;
  reg [3:0]  cs;
  always @(posedge clkin) begin
    cfg_we    <= 1'b0;
    mix_prime <= 1'b0;
    if (por_rst) begin
      cs <= 4'd0; cfg_done <= 1'b0;
    end else begin
      case (cs)
        4'd0: begin cfg_field<=3'd0; cfg_data<=32'd0;          cfg_we<=1; cs<=4'd1; end // step_lo=0
        4'd1: begin cfg_field<=3'd1; cfg_data<=32'd1;          cfg_we<=1; cs<=4'd2; end // step_hi=1 (1.0)
        4'd2: begin cfg_field<=3'd2; cfg_data<=32'h0000000A;   cfg_we<=1; cs<=4'd3; end // flags: active+loop
        4'd3: begin cfg_field<=3'd3; cfg_data<=32'h00007FFF;   cfg_we<=1; cs<=4'd4; end // vol max
        4'd4: begin cfg_field<=3'd4; cfg_data<=32'h00007FFF;   cfg_we<=1; cs<=4'd5; end // pan_l max
        4'd5: begin cfg_field<=3'd5; cfg_data<=32'h00007FFF;   cfg_we<=1; cs<=4'd6; end // pan_r max
        4'd6: begin cfg_field<=3'd6; cfg_data<=32'd128;        cfg_we<=1; cs<=4'd7; end // loop_len=128
        4'd7: begin mix_prime<=1'b1;                                      cs<=4'd8; end
        4'd8: begin if (mix_busy)  cs<=4'd9;  end                                       // prime started
        4'd9: begin if (!mix_busy) begin cfg_done<=1'b1; cs<=4'd10; end end             // prime done
        default: ;
      endcase
    end
  end

  // render one frame per DAC sample tick, once configured and not busy
  always @(posedge clkin) begin
    mix_render <= 1'b0;
    if (!por_rst && cfg_done && sample_req && !mix_busy)
      mix_render <= 1'b1;
  end

  hx_mixer_seq #(.N(8), .CHW(3)) u_mix (
    .clk(clkin), .rst(por_rst),
    .cfg_we(cfg_we), .cfg_ch(3'd0), .cfg_field(cfg_field), .cfg_data(cfg_data),
    .headroom_bits(4'd0), .out_shift(4'd0), .out_offset(32'sd0),
    .out_min(-32'sd32768), .out_max(32'sd32767),
    .start(mix_prime), .render(mix_render),
    .rd_req(rd_req), .rd_ch(rd_ch), .rd_addr(rd_addr), .rd_ack(rd_ack), .rd_data(rd_data),
    .out_l(mix_l), .out_r(mix_r), .out_valid(mix_valid), .busy(mix_busy)
  );

  // hold the finished frame as the DAC input
  always @(posedge clkin) begin
    if (por_rst) begin
      mix_l_r <= 16'sd0; mix_r_r <= 16'sd0;
    end else if (mix_valid) begin
      mix_l_r <= mix_l[15:0];
      mix_r_r <= mix_r[15:0];
    end
  end

  // ---- diagnostics ----
  reg [7:0] tick_cnt=0, mix_cnt=0, sdout_cnt=0;
  reg       tick_seen=0, mix_seen=0, sdout_seen=0, smp_nz=0;
  reg       sdout_d=0;
  always @(posedge clkin) begin
    sdout_d <= sdout;
    if (sample_req)          begin tick_cnt  <= tick_cnt  + 8'd1; tick_seen  <= 1'b1; end
    if (mix_valid)           begin mix_cnt   <= mix_cnt   + 8'd1; mix_seen   <= 1'b1; end
    if (sdout !== sdout_d)    begin sdout_cnt <= sdout_cnt + 8'd1; sdout_seen <= 1'b1; end
    if (dbg_vol_sample != 16'sd0) smp_nz <= 1'b1;
  end
  assign dbg_tick   = tick_cnt;
  assign dbg_mix    = mix_cnt;
  assign dbg_sdout  = sdout_cnt;
  assign dbg_status = {2'b00, cfg_done, smp_nz, sdout_seen, mix_seen, tick_seen};

endmodule
