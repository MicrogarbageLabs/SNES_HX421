// ============================================================
//  hx_produce.v — pipelined per-channel produce datapath
//
//  Splits the combinational cloud that failed timing (cubic chain -> vol -> pan,
//  all in one cycle, Fmax ~20 MHz) into one-multiply-deep registered stages so
//  it holds the 96 MHz memory clock. Values are IDENTICAL to the flat datapath
//  (interp_cubic_q15/interp_linear_q15 + q15_sat_mul); only the timing changes,
//  so the same co-sim re-verifies the mixer bit-for-bit.
//
//  Fixed latency LAT = 8 from `load` to `dvalid`. The sequencer is latency-
//  agnostic (it waits on `dvalid`, not a hard count), so this cost is free given
//  the mixer uses ~190 of 2177 cycles/frame.
//
//  Stages (register at end of each):
//    0 load:  latch inputs
//    1 c_t2 = (t*t)>>15 ; coeffs a,b,c,d ; lerp scaled = (diff*f)>>16
//    2 c_t3 = (t2*t)>>15
//    3 products (a*t3)>>15, (b*t2)>>15, (c*t)>>15
//    3b sums: cubsum = at3+bt2+cct+d ; linsum = p0+scaled  (isolate the 4-way
//        add — it plus sat+mux in one cycle was the 96 MHz critical path in-context)
//    4 cub = sat(cubsum>>1) ; lin = linsum ; -> s_raw = cubic ? cub : lin
//    5 sv  = (vol==ONE) ? s_raw : sat_mul(s_raw,vol)
//    6 samp_l/r = per-side pan with unity bypass
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_produce (
    input  wire        clk,
    input  wire        load,                 // latch inputs, start a produce
    input  wire signed [15:0] p0, p1, p2, p3,
    input  wire        [31:0] frac,
    input  wire        cubic,
    input  wire signed [15:0] vol, pan_l, pan_r,
    output reg  signed [15:0] samp_l,
    output reg  signed [15:0] samp_r,
    output reg         dvalid
);
    localparam signed [15:0] Q15_ONE = 16'sd32767;

    function signed [15:0] sat16; input signed [31:0] v;
        sat16 = (v >  32767) ? 16'sd32767 : (v < -32768) ? -16'sd32768 : v[15:0];
    endfunction
    function signed [15:0] satmul; input signed [15:0] a; input signed [15:0] b;
        satmul = sat16(($signed(a) * $signed(b)) >>> 15);
    endfunction

    // valid pipeline (LAT = 8)
    reg [7:0] vpipe;
    always @(posedge clk) vpipe <= {vpipe[6:0], load};

    // ---- stage 0 -> 1 : inputs registered as s1_* ----
    reg signed [15:0] s1_p0, s1_p1, s1_vol, s1_pl, s1_pr;
    reg        s1_cubic;
    reg signed [31:0] s1_t, s1_t2, s1_a, s1_b, s1_c, s1_d, s1_scaled;
    wire signed [31:0] t0   = $signed({15'd0, frac[31:17]});      // q15, 0..0x7FFF
    wire signed [31:0] tt0  = t0 * t0;
    wire        [15:0] f0   = frac[31:16];
    wire signed [31:0] diff0 = $signed(p1) - $signed(p0);
    always @(posedge clk) begin
        s1_p0 <= p0; s1_p1 <= p1; s1_vol <= vol; s1_pl <= pan_l; s1_pr <= pan_r;
        s1_cubic <= cubic;
        s1_t  <= t0;
        s1_t2 <= tt0 >>> 15;
        s1_a  <= -p0 + 3*p1 - 3*p2 + p3;
        s1_b  <= (p0 <<< 1) - 5*p1 + (p2 <<< 2) - p3;
        s1_c  <= -p0 + p2;
        s1_d  <= p1 <<< 1;
        s1_scaled <= (diff0 * $signed({1'b0, f0})) >>> 16;
    end

    // ---- stage 1 -> 2 : t3 ----
    reg signed [15:0] s2_p0, s2_vol, s2_pl, s2_pr;
    reg        s2_cubic;
    reg signed [31:0] s2_t, s2_t2, s2_t3, s2_a, s2_b, s2_c, s2_d, s2_scaled;
    always @(posedge clk) begin
        s2_p0<=s1_p0; s2_vol<=s1_vol; s2_pl<=s1_pl; s2_pr<=s1_pr; s2_cubic<=s1_cubic;
        s2_t<=s1_t; s2_t2<=s1_t2; s2_a<=s1_a; s2_b<=s1_b; s2_c<=s1_c; s2_d<=s1_d;
        s2_scaled<=s1_scaled;
        s2_t3 <= (s1_t2 * s1_t) >>> 15;
    end

    // ---- stage 2 -> 3 : the three products ----
    reg signed [15:0] s3_p0, s3_vol, s3_pl, s3_pr;
    reg        s3_cubic;
    reg signed [31:0] s3_at3, s3_bt2, s3_cct, s3_d, s3_scaled;
    always @(posedge clk) begin
        s3_p0<=s2_p0; s3_vol<=s2_vol; s3_pl<=s2_pl; s3_pr<=s2_pr; s3_cubic<=s2_cubic;
        s3_d<=s2_d; s3_scaled<=s2_scaled;
        s3_at3 <= (s2_a * s2_t3) >>> 15;
        s3_bt2 <= (s2_b * s2_t2) >>> 15;
        s3_cct <= (s2_c * s2_t)  >>> 15;
    end

    // ---- stage 3 -> 3b : the two 32-bit sums (isolated from sat+mux) ----
    reg signed [15:0] s3b_vol, s3b_pl, s3b_pr;
    reg        s3b_cubic;
    reg signed [31:0] s3b_cubsum, s3b_linsum;
    always @(posedge clk) begin
        s3b_vol<=s3_vol; s3b_pl<=s3_pl; s3b_pr<=s3_pr; s3b_cubic<=s3_cubic;
        s3b_cubsum <= s3_at3 + s3_bt2 + s3_cct + s3_d;
        s3b_linsum <= $signed(s3_p0) + s3_scaled;    // p0 + scaled
    end

    // ---- stage 3b -> 4 : cubic saturate, lerp truncate, select ----
    reg signed [15:0] s4_raw, s4_vol, s4_pl, s4_pr;
    wire signed [15:0] cub_o = sat16(s3b_cubsum >>> 1);
    wire signed [15:0] lin_o = s3b_linsum[15:0];
    always @(posedge clk) begin
        s4_vol<=s3b_vol; s4_pl<=s3b_pl; s4_pr<=s3b_pr;
        s4_raw <= s3b_cubic ? cub_o : lin_o;
    end

    // ---- stage 4 -> 5 : volume ----
    reg signed [15:0] s5_sv, s5_pl, s5_pr;
    always @(posedge clk) begin
        s5_pl<=s4_pl; s5_pr<=s4_pr;
        s5_sv <= (s4_vol == Q15_ONE) ? s4_raw : satmul(s4_raw, s4_vol);
    end

    // ---- stage 5 -> 6 : pan L/R (unity bypass per side) ----
    always @(posedge clk) begin
        samp_l <= (s5_pl == Q15_ONE) ? s5_sv : satmul(s5_sv, s5_pl);
        samp_r <= (s5_pr == Q15_ONE) ? s5_sv : satmul(s5_sv, s5_pr);
        dvalid <= vpipe[7];
    end

endmodule

`default_nettype wire
