// ============================================================
//  hx_cubic.v — Catmull-Rom cubic interpolator, bit-exact to the C mixer
//
//  The golden model is interp_cubic_q15() in engine/audio/audio_mixer.c.
//  This is the mixer's most arithmetic-heavy stage and the one most prone to
//  a fixed-point divergence, so it is built and co-simulated FIRST — the
//  vectors in sim/ run the same inputs through the C and through this and
//  diff bit-for-bit.
//
//  BIT-EXACTNESS, including overflow. The C does every multiply as int32 x
//  int32 -> int32 and shifts with a signed >>. With full-scale alternating
//  taps, a*t3 reaches ~2^33 and WRAPS in the C's int32 — so this reproduces
//  the wrap deliberately: 32-bit signed intermediates, truncate each product
//  to 32 bits before the arithmetic shift, use >>> not >>. Matching the C
//  exactly (rather than "fixing" it with wider math) is what keeps the C a
//  valid oracle; the input range over which the result is trustworthy is
//  characterised separately, not papered over here.
//
//  Purely combinational. Maps onto the fabric's 18x18 hard multipliers.
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_cubic (
    input  wire signed [15:0] p0,     // taps, q15
    input  wire signed [15:0] p1,
    input  wire signed [15:0] p2,
    input  wire signed [15:0] p3,
    input  wire        [31:0] frac_q32,   // fractional phase, q32
    output wire signed [15:0] out         // interpolated sample, q15
);

    // t = frac_q32 >> 17  -> 0..0x7FFF (q15). Unsigned in, small and positive.
    wire signed [31:0] t  = $signed({15'd0, frac_q32[31:17]});

    // t2 = (t*t) >> 15,  t3 = (t2*t) >> 15. Each product truncated to 32 bits
    // first (assign to signed[31:0]) then arithmetic-shifted, exactly as C.
    wire signed [31:0] tt  = t  * t;      // low 32 bits of the product
    wire signed [31:0] t2  = tt  >>> 15;
    wire signed [31:0] t2t = t2 * t;
    wire signed [31:0] t3  = t2t >>> 15;

    // Coefficients. int32 in C; these sums stay well inside 32 bits.
    wire signed [31:0] a = -p0 + 3*p1 - 3*p2 + p3;
    wire signed [31:0] b = (p0 <<< 1) - 5*p1 + (p2 <<< 2) - p3;   // 2p0 -5p1 +4p2 -p3
    wire signed [31:0] c = -p0 + p2;
    wire signed [31:0] d = p1 <<< 1;                              // 2p1

    // v = (a*t3)>>15 + (b*t2)>>15 + (c*t)>>15 + d, each product truncated to
    // 32 bits BEFORE the shift so an overflow wraps identically to the C.
    wire signed [31:0] at3 = a * t3;
    wire signed [31:0] bt2 = b * t2;
    wire signed [31:0] ct  = c * t;

    wire signed [31:0] v_sum = (at3 >>> 15) + (bt2 >>> 15) + (ct >>> 15) + d;

    // The formula is doubled (0.5 factored out); halve, then saturate to q15.
    wire signed [31:0] v = v_sum >>> 1;

    assign out = (v >  32767) ? 16'sd32767
               : (v < -32768) ? -16'sd32768
               : v[15:0];

endmodule

`default_nettype wire
