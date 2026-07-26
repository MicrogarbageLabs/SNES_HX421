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

module hx_mixer_dac #(
  // Loop length of channel 0, in samples. Default 128 = the baked sine. A
  // streaming/long-clip build overrides this to the ring size (e.g. 32768 for a
  // 64 KB mono ring) so the mixer wraps at the ring boundary, not every 128.
  parameter [31:0] LOOP_LEN = 32'd128,
  // When 1, also configure channel 1 (same wavetable, step 1.5 = a perfect fifth
  // above ch0) and drop both channels to half volume -> the mixer sums two tones
  // into a chord: the first on-silicon test of actual multi-channel MIXING. When
  // 0 (default) the FSM is byte-identical to the single-channel sine core.
  parameter SECOND_CH = 1'b0
)(
  input  clkin,       // CLK2, 96 MHz
  input  sysclk,      // SNES_SYSCLK
  input  palmode,
  output sdout,
  output mclk_out,
  output lrck_out,
  // ---- 6b: PSRAM read port (to main.v's MIX_RD ROM-bus requestor) ----
  //  rom_rd_req/addr are the mixer's sample fetches; main.v serves them from
  //  PSRAM and returns rom_rd_data (already byte-order-corrected) + rom_rd_ack.
  output        rom_rd_req,
  output [23:0] rom_rd_addr,     // mixer sample index (word units)
  input         rom_rd_ack,
  input  signed [15:0] rom_rd_data,
  output [23:0] drain_pos,       // channel 0 read position (STM32 drain pointer)
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

  // ---- mixer read port -> external PSRAM (via main.v MIX_RD requestor) ----
  wire        rd_req;
  wire [2:0]  rd_ch;
  wire [31:0] rd_addr;
  assign rom_rd_req  = rd_req;
  assign rom_rd_addr = rd_addr[23:0];       // sample index; main.v scales to bytes
  wire        rd_ack  = rom_rd_ack;
  wire signed [15:0] rd_data = rom_rd_data; // byte-order corrected in main.v

  // mixer outputs (declared before use in the FSM below)
  wire signed [31:0] mix_l, mix_r;
  wire        mix_valid, mix_busy;

  // ---- config / control FSM: set up channel(s), prime, then render per tick ----
  //  Channel 0 always: step 1.0, active+loop, loop_len=LOOP_LEN. If SECOND_CH,
  //  also channel 1 at step 1.5 (a fifth up), and both volumes drop to half so the
  //  summed pair doesn't clip. SECOND_CH=0 -> ch1 states are skipped and ch0 keeps
  //  full volume, so the single-channel core is unchanged.
  localparam [15:0] CH_VOL = SECOND_CH ? 16'h4000 : 16'h7FFF;
  reg        cfg_we;
  reg [2:0]  cfg_ch_r;
  reg [2:0]  cfg_field;
  reg [31:0] cfg_data;
  reg        mix_prime, mix_render, cfg_done;
  reg [4:0]  cs;
  always @(posedge clkin) begin
    cfg_we    <= 1'b0;
    mix_prime <= 1'b0;
    if (por_rst) begin
      cs <= 5'd0; cfg_done <= 1'b0; cfg_ch_r <= 3'd0;
    end else begin
      case (cs)
        // ---- channel 0 ----
        5'd0: begin cfg_ch_r<=3'd0; cfg_field<=3'd0; cfg_data<=32'd0;              cfg_we<=1; cs<=5'd1; end // step_lo=0
        5'd1: begin cfg_field<=3'd1; cfg_data<=32'd1;                              cfg_we<=1; cs<=5'd2; end // step_hi=1 (1.0)
        5'd2: begin cfg_field<=3'd2; cfg_data<=32'h0000000A;                       cfg_we<=1; cs<=5'd3; end // active+loop
        5'd3: begin cfg_field<=3'd3; cfg_data<={16'd0, CH_VOL};                    cfg_we<=1; cs<=5'd4; end // vol
        5'd4: begin cfg_field<=3'd4; cfg_data<=32'h00007FFF;                       cfg_we<=1; cs<=5'd5; end // pan_l max
        5'd5: begin cfg_field<=3'd5; cfg_data<=32'h00007FFF;                       cfg_we<=1; cs<=5'd6; end // pan_r max
        5'd6: begin cfg_field<=3'd6; cfg_data<=LOOP_LEN;   cfg_we<=1; cs<=(SECOND_CH ? 5'd7 : 5'd14); end   // loop_len
        // ---- channel 1 (only if SECOND_CH): step 1.5 = ch0 + a fifth ----
        5'd7:  begin cfg_ch_r<=3'd1; cfg_field<=3'd0; cfg_data<=32'h80000000;      cfg_we<=1; cs<=5'd8;  end // step_lo=0.5
        5'd8:  begin cfg_field<=3'd1; cfg_data<=32'd1;                             cfg_we<=1; cs<=5'd9;  end // step_hi=1 (=>1.5)
        5'd9:  begin cfg_field<=3'd2; cfg_data<=32'h0000000A;                      cfg_we<=1; cs<=5'd10; end // active+loop
        5'd10: begin cfg_field<=3'd3; cfg_data<={16'd0, CH_VOL};                   cfg_we<=1; cs<=5'd11; end // vol
        5'd11: begin cfg_field<=3'd4; cfg_data<=32'h00007FFF;                      cfg_we<=1; cs<=5'd12; end // pan_l max
        5'd12: begin cfg_field<=3'd5; cfg_data<=32'h00007FFF;                      cfg_we<=1; cs<=5'd13; end // pan_r max
        5'd13: begin cfg_field<=3'd6; cfg_data<=LOOP_LEN;                          cfg_we<=1; cs<=5'd14; end // loop_len
        // ---- prime + go ----
        5'd14: begin mix_prime<=1'b1;                                                         cs<=5'd15; end
        5'd15: begin if (mix_busy)  cs<=5'd16;  end                                                          // prime started
        5'd16: begin if (!mix_busy) begin cfg_done<=1'b1; cs<=5'd17; end end                                 // prime done
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
    .cfg_we(cfg_we), .cfg_ch(cfg_ch_r), .cfg_field(cfg_field), .cfg_data(cfg_data),
    .headroom_bits(4'd0), .out_shift(4'd0), .out_offset(32'sd0),
    .out_min(-32'sd32768), .out_max(32'sd32767),
    .start(mix_prime), .render(mix_render),
    .rd_req(rd_req), .rd_ch(rd_ch), .rd_addr(rd_addr), .rd_ack(rd_ack), .rd_data(rd_data),
    .out_l(mix_l), .out_r(mix_r), .out_valid(mix_valid), .busy(mix_busy),
    .pos0(drain_pos)
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
