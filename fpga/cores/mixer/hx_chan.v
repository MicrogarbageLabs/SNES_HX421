// ============================================================
//  hx_chan.v — one-channel resampling datapath (mono), matches the C mixer's
//  produce_channel_sample() for the resample / non-loop path.
//
//  The output SEQUENCE is bit-exact to mixer_render on a single mono channel
//  with unity volume, centre pan, zero headroom, 16-bit-signed output — i.e.
//  the raw interpolated stream. Loop mode and stereo are extensions; this
//  proves the control (phase accumulator + sliding tap window) feeding the
//  already-verified hx_cubic / hx_lerp kernels.
//
//  MODEL. The resample path is a sliding window over the source samples, which
//  are consumed strictly in order. The q32.32 phase accumulates `step` per
//  output frame; when it crosses an integer boundary the window slides left by
//  that many samples and pulls the same number of fresh samples off the source.
//  Priming is split into its own cycle here — it emits no output and does not
//  advance phase, so the produced sequence is identical to the C priming inside
//  its first produce call.
//
//  Faithful to the C down to a corner: the window slide and source consume are
//  clamped to the tap count (>4x downsample can't slide further), but phase
//  subtracts the FULL integer advance. That silently skips source at extreme
//  downsampling — a real, deterministic C behaviour, reproduced rather than
//  "corrected", so the C stays a valid oracle.
//
//  Source read: rd_base drives a 4-wide combinational read (src[base+0..3]).
//  A tight synchronous read models a BRAM/staging fetch; the PSRAM latency is
//  step 6, not here.
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_chan (
    input  wire               clk,
    input  wire               rst,
    input  wire               start,       // prime the tap window (pulse once)
    input  wire               produce,     // emit one output frame (pulse)
    input  wire               cubic,       // 1 = cubic (4 taps), 0 = linear (2)
    input  wire [63:0]        step_q32,    // q32.32 phase step

    output wire [31:0]        rd_base,     // source read base index (combinational)
    input  wire signed [15:0] rd_s0,       // src[rd_base + 0..3]
    input  wire signed [15:0] rd_s1,
    input  wire signed [15:0] rd_s2,
    input  wire signed [15:0] rd_s3,

    output reg  signed [15:0] out,         // produced sample, q15
    output reg                valid        // out valid, pulses with each produce
);

    reg signed [15:0] tap0, tap1, tap2, tap3;
    reg        [63:0] phase;
    reg        [31:0] src_pos;             // index of the next source sample to load
    reg               primed;

    // --- interpolation kernels (already co-sim-proven) ---
    wire signed [15:0] cub_out, lin_out;
    hx_cubic u_cub (.p0(tap0), .p1(tap1), .p2(tap2), .p3(tap3),
                    .frac_q32(phase[31:0]), .out(cub_out));
    hx_lerp  u_lin (.a(tap0), .b(tap1), .frac_q32(phase[31:0]), .out(lin_out));
    wire signed [15:0] interp_out = cubic ? cub_out : lin_out;

    // --- phase advance ---
    wire [2:0]  n_taps    = cubic ? 3'd4 : 3'd2;
    wire [63:0] new_phase = phase + step_q32;
    wire [31:0] n_adv_full = new_phase[63:32];              // whole samples crossed
    wire [2:0]  n_adv = (n_adv_full >= {29'd0, n_taps}) ? n_taps : n_adv_full[2:0];

    // Prime reads src[0..2]; every advance reads from the current src_pos.
    assign rd_base = start ? 32'd0 : src_pos;

    always @(posedge clk) begin
        valid <= 1'b0;
        if (rst) begin
            tap0 <= 0; tap1 <= 0; tap2 <= 0; tap3 <= 0;
            phase <= 0; src_pos <= 0; primed <= 0;
        end else if (start) begin
            // cubic:  taps = [0, s0, s1, s2], next source index 3
            // linear: taps = [s0, s1, -, -],  next source index 2
            if (cubic) begin
                tap0 <= 16'd0; tap1 <= rd_s0; tap2 <= rd_s1; tap3 <= rd_s2;
                src_pos <= 32'd3;
            end else begin
                tap0 <= rd_s0; tap1 <= rd_s1; tap2 <= 16'd0; tap3 <= 16'd0;
                src_pos <= 32'd2;
            end
            phase <= 0; primed <= 1'b1;
        end else if (produce & primed) begin
            out   <= interp_out;
            valid <= 1'b1;

            // slide the window left by n_adv and pull n_adv fresh samples.
            if (cubic) begin
                case (n_adv)
                    3'd0: ;
                    3'd1: begin tap0<=tap1; tap1<=tap2; tap2<=tap3; tap3<=rd_s0; end
                    3'd2: begin tap0<=tap2; tap1<=tap3; tap2<=rd_s0; tap3<=rd_s1; end
                    3'd3: begin tap0<=tap3; tap1<=rd_s0; tap2<=rd_s1; tap3<=rd_s2; end
                    default: begin tap0<=rd_s0; tap1<=rd_s1; tap2<=rd_s2; tap3<=rd_s3; end
                endcase
            end else begin
                case (n_adv)
                    3'd0: ;
                    3'd1: begin tap0<=tap1;  tap1<=rd_s0; end
                    default: begin tap0<=rd_s0; tap1<=rd_s1; end  // n_adv >= 2
                endcase
            end

            if (n_adv > 0) src_pos <= src_pos + {29'd0, n_adv};   // clamped consume
            // subtract the FULL integer advance so frac carries; int part -> 0.
            phase <= new_phase - {n_adv_full, 32'd0};
        end
    end

endmodule

`default_nettype wire
