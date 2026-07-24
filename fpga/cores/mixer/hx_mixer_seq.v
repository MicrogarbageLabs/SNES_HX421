// ============================================================
//  hx_mixer_seq.v — latency-tolerant mixer, one PSRAM port, sequential reads
//
//  The synthesizable mixer: single request/ack read port tolerating real PSRAM
//  latency, output IDENTICAL to hx_mixer (co-simulated against the same golden
//  mixer_render vectors under modelled latency).
//
//  LOAD-CONTEXT ARCHITECTURE (for 96 MHz timing). Per-channel state lives in
//  register arrays, but NO compute path may cross the channel mux/demux or Fmax
//  collapses (the flat combinational version hit ~20 MHz; a partly-pipelined one
//  ~74 MHz, both bottlenecked on ci -> mux -> compute -> array-write). So each
//  channel is processed through flat WORKING registers: S_LOAD latches
//  arrays[cur] -> w_* (the only mux, mux->reg), all arithmetic reads/writes w_*
//  (no mux), S_STORE writes w_* -> arrays[cur] (the only demux, reg->demux).
//  Every compute chain is then reg->reg.
//
//  Datapath uses the pipelined hx_produce (interp -> vol -> pan) and a 2-stage
//  finalize. See docs/audio-fpga-mixer.md.
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

    input  wire        cfg_we,
    input  wire [CHW-1:0] cfg_ch,
    input  wire [2:0]  cfg_field,     // 0 step_lo,1 step_hi,2 flags,3 vol,4 panl,5 panr,6 loop_len
    input  wire [31:0] cfg_data,

    input  wire [3:0]  headroom_bits,
    input  wire [3:0]  out_shift,
    input  wire signed [31:0] out_offset,
    input  wire signed [31:0] out_min,
    input  wire signed [31:0] out_max,

    input  wire        start,
    input  wire        render,

    output wire        rd_req,
    output wire [CHW-1:0] rd_ch,
    output wire [31:0] rd_addr,
    input  wire        rd_ack,
    input  wire signed [15:0] rd_data,

    output reg  signed [31:0] out_l,
    output reg  signed [31:0] out_r,
    output reg         out_valid,
    output wire        busy
);
    localparam signed [15:0] Q15_ONE = 16'sd32767;

    // ---- per-channel CONFIG (written by cfg_*) ----
    reg  [63:0] step    [0:N-1];
    reg         cubic   [0:N-1], active [0:N-1], muted [0:N-1], loopf [0:N-1];
    reg  [23:0] loop_len[0:N-1];
    reg  signed [15:0] vol [0:N-1], pan_l [0:N-1], pan_r [0:N-1];
    // ---- per-channel STATE (owned by the sequencer) ----
    reg  [31:0] phase   [0:N-1];       // q0.32 fractional (integer part always 0)
    reg  signed [15:0] tap0 [0:N-1], tap1 [0:N-1], tap2 [0:N-1], tap3 [0:N-1];
    reg  [23:0] src_pos [0:N-1];

    integer j;
    always @(posedge clk) begin
        if (rst) begin
            for (j=0;j<N;j=j+1) begin
                step[j]<=0; cubic[j]<=0; active[j]<=0; muted[j]<=0; loopf[j]<=0;
                loop_len[j]<=24'd1; vol[j]<=Q15_ONE; pan_l[j]<=Q15_ONE; pan_r[j]<=Q15_ONE;
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

    // ---- sequencer ----
    localparam S_IDLE=4'd0, S_LOAD=4'd1, S_DISP=4'd2, S_PWAIT=4'd3,
               S_ISSUE=4'd4, S_WAIT=4'd5, S_STORE=4'd6,
               S_FINAL=4'd7, S_FINAL2=4'd8, S_FINAL3=4'd9;
    reg [3:0]  state;
    reg        mode_prime;
    reg [CHW:0] ci;
    reg signed [31:0] acc_l, acc_r;
    reg [2:0]  k;

    wire [CHW-1:0] cur = ci[CHW-1:0];
    assign rd_ch = cur;
    assign busy  = (state != S_IDLE);

    // ---- flat WORKING registers for the channel being processed ----
    reg  [31:0] w_phase;
    reg  [23:0] w_src_pos, w_loop_len;
    reg  signed [15:0] w_tap0, w_tap1, w_tap2, w_tap3, w_vol, w_panl, w_panr;
    reg  [63:0] w_step;
    reg         w_cubic, w_active, w_muted, w_loopf;

    // compute wires, all on the flat working regs (no channel mux)
    wire [2:0]  w_ntaps = w_cubic ? 3'd4 : 3'd2;
    wire [1:0]  w_curidx = w_cubic ? 2'd1 : 2'd0;
    wire [32:0] w_frac_sum  = {1'b0, w_phase} + {1'b0, w_step[31:0]};
    wire [31:0] w_nadv_full = w_step[63:32] + {31'd0, w_frac_sum[32]};
    wire [2:0]  w_nadv = (w_nadv_full >= {29'd0, w_ntaps}) ? w_ntaps : w_nadv_full[2:0];
    wire [23:0] w_nsp = (w_loopf && (w_src_pos + 24'd1 >= w_loop_len))
                        ? (w_src_pos + 24'd1 - w_loop_len) : (w_src_pos + 24'd1);

    // Combinational read request: held throughout S_WAIT but drops the SAME
    // cycle rd_ack arrives, so a reader that is not always listening (an arbiter
    // busy with another port) still captures it, and nothing re-triggers on the
    // ack cycle. A registered one-cycle pulse would be missed or double-served.
    assign rd_req  = (state == S_WAIT) && !rd_ack;
    assign rd_addr = {8'd0, w_src_pos};

    // pipelined produce (interp -> vol -> pan), fed from working regs
    reg  prod_load;
    wire signed [15:0] pr_samp_l, pr_samp_r;
    wire        pr_dvalid;
    hx_produce u_prod (
        .clk(clk), .load(prod_load),
        .p0(w_tap0), .p1(w_tap1), .p2(w_tap2), .p3(w_tap3),
        .frac(w_phase), .cubic(w_cubic),
        .vol(w_vol), .pan_l(w_panl), .pan_r(w_panr),
        .samp_l(pr_samp_l), .samp_r(pr_samp_r), .dvalid(pr_dvalid)
    );

    // 3-stage finalize (accumulators are not per-channel, so no mux here):
    //   1 headroom shift + q15 saturate ; 2 out_shift + offset ; 3 clamp.
    reg signed [31:0] fin1_l, fin1_r, fin2_l, fin2_r;
    function signed [31:0] fin_sat; input signed [31:0] v0;
        fin_sat = (v0 > 32767) ? 32'sd32767 : (v0 < -32768) ? -32'sd32768 : v0;
    endfunction
    function signed [31:0] fin_clamp; input signed [31:0] v2;
        fin_clamp = (v2 > out_max) ? out_max : (v2 < out_min) ? out_min : v2;
    endfunction

    always @(posedge clk) begin
        out_valid <= 1'b0;
        prod_load <= 1'b0;
        if (rst) begin
            state <= S_IDLE; ci <= 0; acc_l <= 0; acc_r <= 0; k <= 0;
            for (j=0;j<N;j=j+1) begin
                phase[j]<=0; tap0[j]<=0; tap1[j]<=0; tap2[j]<=0; tap3[j]<=0; src_pos[j]<=0;
            end
        end else begin
            case (state)
                S_IDLE: begin
                    if (start)       begin mode_prime<=1'b1; ci<=0; state<=S_LOAD; end
                    else if (render) begin mode_prime<=1'b0; ci<=0; acc_l<=0; acc_r<=0; state<=S_LOAD; end
                end

                // latch this channel's context into the flat working regs
                // (the ONLY channel mux). Compute happens on w_* afterward.
                S_LOAD: begin
                    w_phase  <= phase[cur];   w_src_pos <= src_pos[cur];
                    w_tap0   <= tap0[cur];     w_tap1 <= tap1[cur];
                    w_tap2   <= tap2[cur];     w_tap3 <= tap3[cur];
                    w_step   <= step[cur];     w_cubic<= cubic[cur];
                    w_active <= active[cur];   w_muted<= muted[cur];
                    w_loopf  <= loopf[cur];    w_loop_len <= loop_len[cur];
                    w_vol    <= vol[cur];      w_panl <= pan_l[cur]; w_panr <= pan_r[cur];
                    state    <= S_DISP;
                end

                S_DISP: begin
                    if (!w_active) begin
                        state <= S_STORE;          // nothing to do; write back unchanged
                    end else if (mode_prime) begin
                        w_tap0<=0; w_tap1<=0; w_tap2<=0; w_tap3<=0;
                        w_src_pos<=0; w_phase<=0;
                        k <= w_ntaps - {1'b0, w_curidx};   // 3 cubic / 2 linear
                        state <= S_ISSUE;
                    end else begin
                        prod_load <= 1'b1;                 // pipeline reads w_*
                        state <= S_PWAIT;
                    end
                end

                S_PWAIT: begin
                    if (pr_dvalid) begin
                        if (!w_muted) begin
                            acc_l <= acc_l + {{16{pr_samp_l[15]}}, pr_samp_l};
                            acc_r <= acc_r + {{16{pr_samp_r[15]}}, pr_samp_r};
                        end
                        w_phase <= w_frac_sum[31:0];       // advance the fractional phase
                        k <= w_nadv;
                        state <= (w_nadv > 0) ? S_ISSUE : S_STORE;
                    end
                end

                S_ISSUE: begin
                    // rd_req/rd_addr are combinational (asserted in S_WAIT).
                    if (k == 0) state <= S_STORE;
                    else        state <= S_WAIT;
                end

                S_WAIT: begin
                    if (rd_ack) begin
                        if (w_cubic) begin
                            w_tap0<=w_tap1; w_tap1<=w_tap2; w_tap2<=w_tap3; w_tap3<=rd_data;
                        end else begin
                            w_tap0<=w_tap1; w_tap1<=rd_data;
                        end
                        w_src_pos <= w_nsp;
                        k <= k - 1;
                        state <= S_ISSUE;
                    end
                end

                // write the working context back (the ONLY channel demux)
                S_STORE: begin
                    phase[cur]   <= w_phase;
                    src_pos[cur] <= w_src_pos;
                    tap0[cur]<=w_tap0; tap1[cur]<=w_tap1; tap2[cur]<=w_tap2; tap3[cur]<=w_tap3;
                    ci <= ci + 1;
                    if (ci == N-1) state <= mode_prime ? S_IDLE : S_FINAL;
                    else           state <= S_LOAD;
                end

                S_FINAL: begin
                    fin1_l <= fin_sat(acc_l >>> headroom_bits);
                    fin1_r <= fin_sat(acc_r >>> headroom_bits);
                    state  <= S_FINAL2;
                end

                S_FINAL2: begin
                    fin2_l <= (fin1_l >>> out_shift) + out_offset;
                    fin2_r <= (fin1_r >>> out_shift) + out_offset;
                    state  <= S_FINAL3;
                end

                S_FINAL3: begin
                    out_l <= fin_clamp(fin2_l);
                    out_r <= fin_clamp(fin2_r);
                    out_valid <= 1'b1;
                    state <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
