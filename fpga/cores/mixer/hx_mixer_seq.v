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

    // per-channel state + config. phase is the FRACTIONAL part only (q0.32): the
    // C keeps the integer part at 0 each frame, so a 32-bit frac + a tiny carry
    // is exactly equivalent and avoids a 64-bit add on the critical path.
    reg  [31:0] phase   [0:N-1];
    reg  signed [15:0] tap0 [0:N-1], tap1 [0:N-1], tap2 [0:N-1], tap3 [0:N-1];
    // sample indices: 24 bits (16M samples/channel, ~6 min at 44.1 kHz) is
    // ample and keeps the loop-wrap add/compare/subtract off the critical path.
    reg  [23:0] src_pos [0:N-1];
    reg  [63:0] step    [0:N-1];
    reg         cubic   [0:N-1], active [0:N-1], muted [0:N-1], loopf [0:N-1];
    reg  [23:0] loop_len[0:N-1];
    reg  signed [15:0] vol [0:N-1], pan_l [0:N-1], pan_r [0:N-1];

    // CONFIG registers only. The STATE registers (phase, tap*, src_pos) are
    // owned by the sequencer block below — writing them here too would be a
    // multiple-driver net (illegal for synthesis; iverilog silently allowed it).
    integer j;
    always @(posedge clk) begin
        if (rst) begin
            for (j=0;j<N;j=j+1) begin
                step[j]<=0; cubic[j]<=0; active[j]<=0; muted[j]<=0;
                loopf[j]<=0; loop_len[j]<=24'd1; vol[j]<=Q15_ONE; pan_l[j]<=Q15_ONE; pan_r[j]<=Q15_ONE;
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
                3'd6: loop_len[cfg_ch]<= cfg_data[23:0];
                default: ;
            endcase
        end
    end

    // sequencer
    localparam S_IDLE=3'd0, S_SETUP=3'd1, S_ISSUE=3'd2, S_WAIT=3'd3, S_NEXT=3'd4,
               S_FINAL=3'd5, S_PWAIT=3'd6, S_FINAL2=3'd7;
    reg [2:0]  state;
    reg        mode_prime;            // 1 = priming, 0 = producing
    reg [CHW:0] ci;
    reg signed [31:0] acc_l, acc_r;
    reg [2:0]  k;                     // shift-loads remaining for this channel

    wire [CHW-1:0] cur = ci[CHW-1:0];
    assign rd_ch = cur;
    assign busy  = (state != S_IDLE);

    // pipelined produce datapath (interp -> vol -> pan), latency 7, so it holds
    // the 96 MHz clock. One channel in flight at a time (the sequencer serializes).
    reg  prod_load;
    wire signed [15:0] pr_samp_l, pr_samp_r;
    wire        pr_dvalid;
    hx_produce u_prod (
        .clk(clk), .load(prod_load),
        .p0(tap0[cur]), .p1(tap1[cur]), .p2(tap2[cur]), .p3(tap3[cur]),
        .frac(phase[cur]), .cubic(cubic[cur]),
        .vol(vol[cur]), .pan_l(pan_l[cur]), .pan_r(pan_r[cur]),
        .samp_l(pr_samp_l), .samp_r(pr_samp_r), .dvalid(pr_dvalid)
    );

    wire [2:0]  n_taps    = cubic[cur] ? 3'd4 : 3'd2;
    wire [1:0]  cur_idx   = cubic[cur] ? 2'd1 : 2'd0;
    // 32-bit fractional add; the carry plus step's integer part give the whole-
    // sample advance. Equivalent to the C's 64-bit phase+step then -n_adv<<32.
    wire [32:0] frac_sum  = {1'b0, phase[cur]} + {1'b0, step[cur][31:0]};
    wire [31:0] n_adv_full = step[cur][63:32] + {31'd0, frac_sum[32]};
    wire [2:0]  n_adv = (n_adv_full >= {29'd0,n_taps}) ? n_taps : n_adv_full[2:0];

    // finalize, pipelined into two stages (it was the critical path when flat):
    //   stage 1 = headroom shift + q15 saturate  -> fin1_l/r
    //   stage 2 = output-format shift + offset + clamp -> out_l/r
    reg signed [31:0] fin1_l, fin1_r;
    function signed [31:0] fin_sat; input signed [31:0] v0;
        fin_sat = (v0 > 32767) ? 32'sd32767 : (v0 < -32768) ? -32'sd32768 : v0;
    endfunction
    function signed [31:0] fin_clamp; input signed [31:0] v1;
        reg signed [31:0] v2;
        begin
            v2 = (v1 >>> out_shift) + out_offset;
            fin_clamp = (v2 > out_max) ? out_max : (v2 < out_min) ? out_min : v2;
        end
    endfunction

    // src_pos+1 with loop wrap
    wire [23:0] nsp = (loopf[cur] && (src_pos[cur]+24'd1 >= loop_len[cur]))
                      ? (src_pos[cur]+24'd1 - loop_len[cur]) : (src_pos[cur]+24'd1);

    always @(posedge clk) begin
        out_valid <= 1'b0;
        rd_req    <= 1'b0;
        prod_load <= 1'b0;
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
                        src_pos[cur] <= 24'd0;
                        phase[cur]   <= 0;
                        k <= n_taps - {1'b0,cur_idx};
                        state <= S_ISSUE;
                    end else begin
                        // produce: kick the pipeline for this channel, wait for it,
                        // accumulate, then advance. (cur / taps / phase are stable
                        // through S_PWAIT, so no stash is needed.)
                        prod_load <= 1'b1;
                        state <= S_PWAIT;
                    end
                end

                S_PWAIT: begin
                    if (pr_dvalid) begin
                        if (!muted[cur]) begin
                            acc_l <= acc_l + {{16{pr_samp_l[15]}}, pr_samp_l};
                            acc_r <= acc_r + {{16{pr_samp_r[15]}}, pr_samp_r};
                        end
                        k <= n_adv;
                        state <= (n_adv > 0) ? S_ISSUE : S_NEXT;
                    end
                end

                S_ISSUE: begin
                    if (k == 0) begin
                        state <= S_NEXT;
                    end else begin
                        rd_req  <= 1'b1;
                        rd_addr <= {8'd0, src_pos[cur]};  // in [0,loop_len) for loop
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
                    // phase[cur] is untouched until here, so combinational
                    // new_phase / n_adv_full still describe this channel's advance.
                    if (!mode_prime && active[cur])
                        phase[cur] <= frac_sum[31:0];   // integer part stays 0
                    ci <= ci + 1;
                    if (ci == N-1) state <= mode_prime ? S_IDLE : S_FINAL;
                    else           state <= S_SETUP;
                end

                S_FINAL: begin
                    fin1_l <= fin_sat(acc_l >>> headroom_bits);
                    fin1_r <= fin_sat(acc_r >>> headroom_bits);
                    state  <= S_FINAL2;
                end

                S_FINAL2: begin
                    out_l <= fin_clamp(fin1_l);
                    out_r <= fin_clamp(fin1_r);
                    out_valid <= 1'b1;
                    state <= S_IDLE;
                end
            endcase
        end
    end

endmodule

`default_nettype wire
