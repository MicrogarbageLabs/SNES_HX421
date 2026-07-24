// ============================================================
//  hx_mixer_seq.v — latency-tolerant mixer, one PSRAM port, sequential reads
//
//  The synthesizable evolution of hx_mixer.v: the combinational 4-wide source
//  read is replaced by a single request/ack read port that tolerates real PSRAM
//  latency (measured ~7 cycles). The output is IDENTICAL to hx_mixer — latency
//  changes timing, not values — so it is co-simulated against the same golden
//  mixer_render vectors with a latency-modelled PSRAM in the testbench.
//
//  KEY UNIFICATION. Priming and advancing are the same operation: "shift one
//  sample into the tap window from src_pos, then src_pos++". Priming for cubic
//  is zeroed taps + 3 shift-loads (src 0,1,2) which builds [0,s0,s1,s2]
//  naturally; linear is 2 shift-loads -> [s0,s1]. Advancing is n_adv
//  shift-loads. So the whole datapath is a stream of single-sample reads through
//  ONE port — which is exactly what the shared PSRAM bus provides, and it needs
//  no 4-wide read.
//
//  Read handshake: assert rd_req with (rd_ch, rd_addr); the caller returns
//  rd_ack + rd_data some cycles later (models PSRAM/arbiter latency). One read
//  outstanding at a time.
//
//  Timing budget: ~1 read/channel/frame steady state (upsampling), 8 channels,
//  ~7 cycles/read -> ~60 cycles/frame at 96 MHz = 0.6 us against the 22.7 us
//  sample period. The headroom the bandwidth analysis predicted.
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_mixer_seq #(
    parameter integer N = 8,
    parameter integer CHW = 3
) (
    input  wire        clk,
    input  wire        rst,

    // per-channel config (same map as hx_mixer)
    input  wire        cfg_we,
    input  wire [CHW-1:0] cfg_ch,
    input  wire [2:0]  cfg_field,     // 0 step_lo,1 step_hi,2 flags,3 vol,4 panl,5 panr,6 loop_len
    input  wire [31:0] cfg_data,

    // finalize / format
    input  wire [3:0]  headroom_bits,
    input  wire [3:0]  out_shift,
    input  wire signed [31:0] out_offset,
    input  wire signed [31:0] out_min,
    input  wire signed [31:0] out_max,

    input  wire        start,         // prime all active channels
    input  wire        render,        // produce one output frame

    // single-sample read port (request/ack; models PSRAM latency)
    output reg         rd_req,
    output wire [CHW-1:0] rd_ch,
    output reg  [31:0] rd_addr,
    input  wire        rd_ack,
    input  wire signed [15:0] rd_data,

    output reg  signed [31:0] out_l,
    output reg  signed [31:0] out_r,
    output reg         out_valid,
    output wire        busy
);
    localparam signed [15:0] Q15_ONE = 16'sd32767;

    // per-channel state + config
    reg  [63:0] phase   [0:N-1];
    reg  signed [15:0] tap0 [0:N-1], tap1 [0:N-1], tap2 [0:N-1], tap3 [0:N-1];
    reg  [31:0] src_pos [0:N-1];
    reg  [63:0] step    [0:N-1];
    reg         cubic   [0:N-1], active [0:N-1], muted [0:N-1], loopf [0:N-1];
    reg  [31:0] loop_len[0:N-1];
    reg  signed [15:0] vol [0:N-1], pan_l [0:N-1], pan_r [0:N-1];

    // CONFIG registers only. The STATE registers (phase, tap*, src_pos) are
    // owned by the sequencer block below — writing them here too would be a
    // multiple-driver net (illegal for synthesis; iverilog silently allowed it).
    integer j;
    always @(posedge clk) begin
        if (rst) begin
            for (j=0;j<N;j=j+1) begin
                step[j]<=0; cubic[j]<=0; active[j]<=0; muted[j]<=0;
                loopf[j]<=0; loop_len[j]<=32'd1; vol[j]<=Q15_ONE; pan_l[j]<=Q15_ONE; pan_r[j]<=Q15_ONE;
            end
        end else if (cfg_we) begin
            case (cfg_field)
                3'd0: step[cfg_ch][31:0]  <= cfg_data;
                3'd1: step[cfg_ch][63:32] <= cfg_data;
                3'd2: begin cubic[cfg_ch]<=cfg_data[0]; active[cfg_ch]<=cfg_data[1];
                            muted[cfg_ch]<=cfg_data[2]; loopf[cfg_ch]<=cfg_data[3]; end
                3'd3: vol[cfg_ch]     <= cfg_data[15:0];
                3'd4: pan_l[cfg_ch]   <= cfg_data[15:0];
                3'd5: pan_r[cfg_ch]   <= cfg_data[15:0];
                3'd6: loop_len[cfg_ch]<= cfg_data;
                default: ;
            endcase
        end
    end

    // sequencer
    localparam S_IDLE=3'd0, S_SETUP=3'd1, S_ISSUE=3'd2, S_WAIT=3'd3, S_NEXT=3'd4, S_FINAL=3'd5;
    reg [2:0]  state;
    reg        mode_prime;            // 1 = priming, 0 = producing
    reg [CHW:0] ci;
    reg signed [31:0] acc_l, acc_r;
    reg [2:0]  k;                     // shift-loads remaining for this channel
    reg [63:0] stash_new_phase;
    reg [31:0] stash_nadv_full;

    wire [CHW-1:0] cur = ci[CHW-1:0];
    assign rd_ch = cur;
    assign busy  = (state != S_IDLE);

    // combinational datapath on the current channel
    wire signed [15:0] cub_out, lin_out;
    hx_cubic u_cub (.p0(tap0[cur]),.p1(tap1[cur]),.p2(tap2[cur]),.p3(tap3[cur]),
                    .frac_q32(phase[cur][31:0]),.out(cub_out));
    hx_lerp  u_lin (.a(tap0[cur]),.b(tap1[cur]),.frac_q32(phase[cur][31:0]),.out(lin_out));
    wire signed [15:0] s_raw = cubic[cur] ? cub_out : lin_out;

    wire signed [15:0] sv_out, pl_out, pr_out;
    hx_scale u_vol (.a(s_raw),.b(vol[cur]),.out(sv_out));
    wire signed [15:0] sv = (vol[cur]==Q15_ONE) ? s_raw : sv_out;
    hx_scale u_pl (.a(sv),.b(pan_l[cur]),.out(pl_out));
    hx_scale u_pr (.a(sv),.b(pan_r[cur]),.out(pr_out));
    wire signed [15:0] samp_l = (pan_l[cur]==Q15_ONE) ? sv : pl_out;
    wire signed [15:0] samp_r = (pan_r[cur]==Q15_ONE) ? sv : pr_out;

    wire [2:0]  n_taps    = cubic[cur] ? 3'd4 : 3'd2;
    wire [1:0]  cur_idx   = cubic[cur] ? 2'd1 : 2'd0;
    wire [63:0] new_phase = phase[cur] + step[cur];
    wire [31:0] n_adv_full = new_phase[63:32];
    wire [2:0]  n_adv = (n_adv_full >= {29'd0,n_taps}) ? n_taps : n_adv_full[2:0];

    wire signed [31:0] fin_l, fin_r;
    hx_finalize u_fl (.accum(acc_l),.headroom_bits(headroom_bits),.out_shift(out_shift),
                      .out_offset(out_offset),.out_min(out_min),.out_max(out_max),.out(fin_l));
    hx_finalize u_fr (.accum(acc_r),.headroom_bits(headroom_bits),.out_shift(out_shift),
                      .out_offset(out_offset),.out_min(out_min),.out_max(out_max),.out(fin_r));

    // src_pos+1 with loop wrap
    wire [31:0] nsp = (loopf[cur] && (src_pos[cur]+32'd1 >= loop_len[cur]))
                      ? (src_pos[cur]+32'd1 - loop_len[cur]) : (src_pos[cur]+32'd1);

    always @(posedge clk) begin
        out_valid <= 1'b0;
        rd_req    <= 1'b0;
        if (rst) begin
            state <= S_IDLE; ci <= 0; acc_l <= 0; acc_r <= 0; k <= 0;
            for (j=0;j<N;j=j+1) begin
                phase[j]<=0; tap0[j]<=0; tap1[j]<=0; tap2[j]<=0; tap3[j]<=0; src_pos[j]<=0;
            end
        end else begin
            case (state)
                S_IDLE: begin
                    if (start)       begin mode_prime<=1'b1; ci<=0; state<=S_SETUP; end
                    else if (render) begin mode_prime<=1'b0; ci<=0; acc_l<=0; acc_r<=0; state<=S_SETUP; end
                end

                S_SETUP: begin
                    if (!active[cur]) begin
                        state <= S_NEXT;
                    end else if (mode_prime) begin
                        // zero the window, then (n_taps - cur_idx) shift-loads from src 0..
                        tap0[cur]<=0; tap1[cur]<=0; tap2[cur]<=0; tap3[cur]<=0;
                        src_pos[cur] <= 0;
                        phase[cur]   <= 0;
                        k <= n_taps - {1'b0,cur_idx};
                        state <= S_ISSUE;
                    end else begin
                        // produce: interp current window, accumulate, then advance n_adv
                        if (!muted[cur]) begin
                            acc_l <= acc_l + {{16{samp_l[15]}}, samp_l};
                            acc_r <= acc_r + {{16{samp_r[15]}}, samp_r};
                        end
                        stash_new_phase <= new_phase;
                        stash_nadv_full <= n_adv_full;
                        k <= n_adv;
                        state <= (n_adv > 0) ? S_ISSUE : S_NEXT;
                    end
                end

                S_ISSUE: begin
                    if (k == 0) begin
                        state <= S_NEXT;
                    end else begin
                        rd_req  <= 1'b1;
                        rd_addr <= src_pos[cur];      // already in [0,loop_len) for loop
                        state   <= S_WAIT;
                    end
                end

                S_WAIT: begin
                    if (rd_ack) begin
                        // shift one sample into the window
                        if (cubic[cur]) begin
                            tap0[cur]<=tap1[cur]; tap1[cur]<=tap2[cur]; tap2[cur]<=tap3[cur]; tap3[cur]<=rd_data;
                        end else begin
                            tap0[cur]<=tap1[cur]; tap1[cur]<=rd_data;
                        end
                        src_pos[cur] <= nsp;
                        k <= k - 1;
                        state <= S_ISSUE;
                    end
                end

                S_NEXT: begin
                    if (!mode_prime && active[cur])
                        phase[cur] <= stash_new_phase - {stash_nadv_full, 32'd0};
                    ci <= ci + 1;
                    if (ci == N-1) state <= mode_prime ? S_IDLE : S_FINAL;
                    else           state <= S_SETUP;
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
