// ============================================================
//  hx_audio_top.v — free-running audio subsystem: tick -> render -> sample
//
//  Wraps the timing-closed hx_mixer_seq into the shape the hardware needs: a
//  sample tick (master/TICK_DIV ~= 44.1 kHz) fires one mixer render, and the
//  finalized stereo frame is latched as the next DAC sample. The read port
//  passes through to the PSRAM arbiter; audio_l/r + audio_stb drive dac.v.
//
//  The property this exists to guarantee: the mixer ALWAYS finishes a render
//  before the next tick, so no sample is ever missed. `underrun` latches if a
//  tick arrives while the mixer is still busy — proven 0 in sim, since a frame
//  is ~185 cycles against TICK_DIV ~= 2177. A stalled render must not shift the
//  sample phase, so on underrun the tick is still consumed and the previous
//  sample held (a click, not drift).
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_audio_top #(
    parameter integer N = 8,
    parameter integer CHW = 3,
    parameter integer TICK_DIV = 2177     // 96 MHz / 44100 ~= 2177
) (
    input  wire        clk,
    input  wire        rst,

    // config + format pass-through to the mixer
    input  wire        cfg_we,
    input  wire [CHW-1:0] cfg_ch,
    input  wire [2:0]  cfg_field,
    input  wire [31:0] cfg_data,
    input  wire [3:0]  headroom_bits,
    input  wire [3:0]  out_shift,
    input  wire signed [31:0] out_offset,
    input  wire signed [31:0] out_min,
    input  wire signed [31:0] out_max,

    input  wire        prime,              // one-shot: prime all active channels
    input  wire        run,                // 1 = generate ticks and render

    // PSRAM read port (to the arbiter)
    output wire        rd_req,
    output wire [CHW-1:0] rd_ch,
    output wire [31:0] rd_addr,
    input  wire        rd_ack,
    input  wire signed [15:0] rd_data,

    // audio output (to dac.v)
    output reg  signed [15:0] audio_l,
    output reg  signed [15:0] audio_r,
    output reg         audio_stb,          // pulses one clk when a new sample lands
    output reg         underrun            // sticky: a tick was missed
);
    // sample tick divider
    reg [$clog2(TICK_DIV):0] tick_cnt;
    wire tick = run && (tick_cnt == TICK_DIV-1);

    // mixer
    wire signed [31:0] mix_l, mix_r;
    wire        mix_valid, mix_busy;
    reg         mix_render;
    hx_mixer_seq #(.N(N), .CHW(CHW)) u_mix (
        .clk(clk), .rst(rst),
        .cfg_we(cfg_we), .cfg_ch(cfg_ch), .cfg_field(cfg_field), .cfg_data(cfg_data),
        .headroom_bits(headroom_bits), .out_shift(out_shift), .out_offset(out_offset),
        .out_min(out_min), .out_max(out_max),
        .start(prime), .render(mix_render),
        .rd_req(rd_req), .rd_ch(rd_ch), .rd_addr(rd_addr), .rd_ack(rd_ack), .rd_data(rd_data),
        .out_l(mix_l), .out_r(mix_r), .out_valid(mix_valid), .busy(mix_busy)
    );

    always @(posedge clk) begin
        mix_render <= 1'b0;
        audio_stb  <= 1'b0;
        if (rst) begin
            tick_cnt <= 0; underrun <= 0; audio_l <= 0; audio_r <= 0;
        end else begin
            // tick counter
            if (!run)      tick_cnt <= 0;
            else if (tick) tick_cnt <= 0;
            else           tick_cnt <= tick_cnt + 1'b1;

            // on each tick, fire a render — unless the mixer is still busy from
            // the previous one (a missed deadline). The tick is consumed either
            // way so the sample phase never shifts.
            if (tick) begin
                if (mix_busy) underrun <= 1'b1;
                else          mix_render <= 1'b1;
            end

            // latch the finished frame as the next DAC sample
            if (mix_valid) begin
                audio_l   <= mix_l[15:0];
                audio_r   <= mix_r[15:0];
                audio_stb <= 1'b1;
            end
        end
    end

endmodule

`default_nettype wire
