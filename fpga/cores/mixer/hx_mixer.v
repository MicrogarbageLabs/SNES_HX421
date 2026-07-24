// ============================================================
//  hx_mixer.v — time-multiplexed N-channel mixer core
//
//  The full render path, composing the co-sim-proven pieces: per-channel
//  resample (hx_chan's sliding-window model), volume + pan (hx_scale, with the
//  C's Q15_ONE unity-bypass), accumulate, and finalize (hx_finalize). One
//  datapath iterates all channels per output frame — the hardware win from
//  dropping the RISC-V core — so a single cubic/lerp/scale/finalize instance
//  serves every voice.
//
//  Bit-exact to mixer_render for: mono sources -> stereo output, resample /
//  non-loop path, per-channel volume/pan/active/muted, any headroom + output
//  format. Loop mode is a later extension.
//
//  Mono-source simplification: a mono source has tap_l == tap_r, so the produced
//  L and R are identical before pan; only one tap window per channel is kept and
//  the L/R split happens at the pan multiply, exactly as the C's
//  "same sample to L and R" then per-side pan gain.
//
//  Sequencing:
//    start  -> PRIME  : prime every active channel's tap window (1 cycle each)
//    render -> PRODUCE: iterate channels, produce+scale+accumulate (1 cyc each)
//              FINAL  : finalize accum_l/accum_r -> out, pulse out_valid
//
//  Source read: (rd_ch, rd_base) selects a channel's window position; the caller
//  returns src[rd_ch][rd_base + 0..3] combinationally (BRAM-style). PSRAM
//  latency is step 6.
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_mixer #(
    parameter integer N = 8,
    parameter integer CHW = 3            // clog2(N)
) (
    input  wire        clk,
    input  wire        rst,

    // --- per-channel config write (before rendering) ---
    input  wire        cfg_we,
    input  wire [CHW-1:0] cfg_ch,
    input  wire [2:0]  cfg_field,        // 0 step_lo,1 step_hi,2 flags,3 vol,4 pan_l,5 pan_r
    input  wire [31:0] cfg_data,

    // --- output format / headroom (finalize) ---
    input  wire [3:0]  headroom_bits,
    input  wire [3:0]  out_shift,
    input  wire signed [31:0] out_offset,
    input  wire signed [31:0] out_min,
    input  wire signed [31:0] out_max,

    // --- control ---
    input  wire        start,            // prime all active channels
    input  wire        render,           // produce one output frame

    // --- source read (combinational, caller-provided) ---
    output wire [CHW-1:0] rd_ch,
    output wire [31:0]    rd_base,
    input  wire signed [15:0] rd_s0,
    input  wire signed [15:0] rd_s1,
    input  wire signed [15:0] rd_s2,
    input  wire signed [15:0] rd_s3,

    // --- finalized output frame ---
    output reg  signed [31:0] out_l,
    output reg  signed [31:0] out_r,
    output reg         out_valid
);
    localparam Q15_ONE = 16'sd32767;

    // ---- per-channel state ----
    reg  [63:0] phase   [0:N-1];
    reg  signed [15:0] tap0 [0:N-1], tap1 [0:N-1], tap2 [0:N-1], tap3 [0:N-1];
    reg  [31:0] src_pos [0:N-1];
    // ---- per-channel config ----
    reg  [63:0] step    [0:N-1];
    reg         cubic   [0:N-1];
    reg         active  [0:N-1];
    reg         muted   [0:N-1];
    reg  signed [15:0] vol   [0:N-1];
    reg  signed [15:0] pan_l [0:N-1];
    reg  signed [15:0] pan_r [0:N-1];

    integer j;
    always @(posedge clk) begin
        if (rst) begin
            for (j = 0; j < N; j = j + 1) begin
                phase[j]<=0; tap0[j]<=0; tap1[j]<=0; tap2[j]<=0; tap3[j]<=0;
                src_pos[j]<=0; step[j]<=0; cubic[j]<=0; active[j]<=0; muted[j]<=0;
                vol[j]<=Q15_ONE; pan_l[j]<=Q15_ONE; pan_r[j]<=Q15_ONE;
            end
        end else if (cfg_we) begin
            case (cfg_field)
                3'd0: step[cfg_ch][31:0]  <= cfg_data;
                3'd1: step[cfg_ch][63:32] <= cfg_data;
                3'd2: begin cubic[cfg_ch]<=cfg_data[0]; active[cfg_ch]<=cfg_data[1]; muted[cfg_ch]<=cfg_data[2]; end
                3'd3: vol[cfg_ch]   <= cfg_data[15:0];
                3'd4: pan_l[cfg_ch] <= cfg_data[15:0];
                3'd5: pan_r[cfg_ch] <= cfg_data[15:0];
                default: ;
            endcase
        end
    end

    // ---- sequencer ----
    localparam S_IDLE=2'd0, S_PRIME=2'd1, S_PROD=2'd2, S_FINAL=2'd3;
    reg [1:0]  state;
    reg [CHW:0] ci;                       // channel index (one extra bit for ==N)
    reg signed [31:0] acc_l, acc_r;

    wire [CHW-1:0] cur = ci[CHW-1:0];

    // read address: PRIME reads from 0, PRODUCE from the channel's src_pos
    assign rd_ch   = cur;
    assign rd_base = (state == S_PRIME) ? 32'd0 : src_pos[cur];

    // ---- shared datapath (combinational, on the current channel) ----
    wire signed [15:0] cub_out, lin_out;
    hx_cubic u_cub (.p0(tap0[cur]), .p1(tap1[cur]), .p2(tap2[cur]), .p3(tap3[cur]),
                    .frac_q32(phase[cur][31:0]), .out(cub_out));
    hx_lerp  u_lin (.a(tap0[cur]), .b(tap1[cur]), .frac_q32(phase[cur][31:0]), .out(lin_out));
    wire signed [15:0] s_raw = cubic[cur] ? cub_out : lin_out;

    // volume then pan, each with Q15_ONE unity bypass (matches mixer_render)
    wire signed [15:0] sv_out, pl_out, pr_out;
    hx_scale u_vol (.a(s_raw), .b(vol[cur]),   .out(sv_out));
    wire signed [15:0] sv = (vol[cur]   == Q15_ONE) ? s_raw : sv_out;
    hx_scale u_pl  (.a(sv),    .b(pan_l[cur]), .out(pl_out));
    hx_scale u_pr  (.a(sv),    .b(pan_r[cur]), .out(pr_out));
    wire signed [15:0] samp_l = (pan_l[cur] == Q15_ONE) ? sv : pl_out;
    wire signed [15:0] samp_r = (pan_r[cur] == Q15_ONE) ? sv : pr_out;

    // phase advance for the current channel
    wire [2:0]  n_taps    = cubic[cur] ? 3'd4 : 3'd2;
    wire [63:0] new_phase = phase[cur] + step[cur];
    wire [31:0] n_adv_full = new_phase[63:32];
    wire [2:0]  n_adv = (n_adv_full >= {29'd0, n_taps}) ? n_taps : n_adv_full[2:0];

    // finalize (only used in S_FINAL)
    wire signed [31:0] fin_l, fin_r;
    hx_finalize u_fl (.accum(acc_l), .headroom_bits(headroom_bits), .out_shift(out_shift),
                      .out_offset(out_offset), .out_min(out_min), .out_max(out_max), .out(fin_l));
    hx_finalize u_fr (.accum(acc_r), .headroom_bits(headroom_bits), .out_shift(out_shift),
                      .out_offset(out_offset), .out_min(out_min), .out_max(out_max), .out(fin_r));

    always @(posedge clk) begin
        out_valid <= 1'b0;
        if (rst) begin
            state <= S_IDLE; ci <= 0; acc_l <= 0; acc_r <= 0;
        end else begin
            case (state)
                S_IDLE: begin
                    if (start)  begin ci <= 0; state <= S_PRIME;  end
                    else if (render) begin ci <= 0; acc_l <= 0; acc_r <= 0; state <= S_PROD; end
                end

                S_PRIME: begin
                    if (active[cur]) begin
                        if (cubic[cur]) begin
                            tap0[cur]<=16'd0; tap1[cur]<=rd_s0; tap2[cur]<=rd_s1; tap3[cur]<=rd_s2;
                            src_pos[cur]<=32'd3;
                        end else begin
                            tap0[cur]<=rd_s0; tap1[cur]<=rd_s1; tap2[cur]<=16'd0; tap3[cur]<=16'd0;
                            src_pos[cur]<=32'd2;
                        end
                        phase[cur]<=0;
                    end
                    ci <= ci + 1;
                    if (ci == N-1) state <= S_IDLE;
                end

                S_PROD: begin
                    if (active[cur]) begin
                        if (!muted[cur]) begin
                            acc_l <= acc_l + {{16{samp_l[15]}}, samp_l};
                            acc_r <= acc_r + {{16{samp_r[15]}}, samp_r};
                        end
                        // advance this channel's window
                        if (cubic[cur]) begin
                            case (n_adv)
                                3'd0: ;
                                3'd1: begin tap0[cur]<=tap1[cur]; tap1[cur]<=tap2[cur]; tap2[cur]<=tap3[cur]; tap3[cur]<=rd_s0; end
                                3'd2: begin tap0[cur]<=tap2[cur]; tap1[cur]<=tap3[cur]; tap2[cur]<=rd_s0; tap3[cur]<=rd_s1; end
                                3'd3: begin tap0[cur]<=tap3[cur]; tap1[cur]<=rd_s0; tap2[cur]<=rd_s1; tap3[cur]<=rd_s2; end
                                default: begin tap0[cur]<=rd_s0; tap1[cur]<=rd_s1; tap2[cur]<=rd_s2; tap3[cur]<=rd_s3; end
                            endcase
                        end else begin
                            case (n_adv)
                                3'd0: ;
                                3'd1: begin tap0[cur]<=tap1[cur]; tap1[cur]<=rd_s0; end
                                default: begin tap0[cur]<=rd_s0; tap1[cur]<=rd_s1; end
                            endcase
                        end
                        if (n_adv > 0) src_pos[cur] <= src_pos[cur] + {29'd0, n_adv};
                        phase[cur] <= new_phase - {n_adv_full, 32'd0};
                    end
                    ci <= ci + 1;
                    if (ci == N-1) state <= S_FINAL;
                end

                S_FINAL: begin
                    out_l <= fin_l; out_r <= fin_r; out_valid <= 1'b1;
                    state <= S_IDLE;
                end
            endcase
        end
    end

endmodule

`default_nettype wire
