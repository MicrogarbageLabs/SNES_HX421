// ============================================================
//  hx_lerp.v — linear interpolator, bit-exact to interp_linear_q15
//
//  Golden model (engine/audio/audio_mixer.c):
//    uint32_t f = frac_q32 >> 16;         // q16, 0..0xFFFF
//    int32_t  diff = (int32_t)b - (int32_t)a;
//    int32_t  scaled = (diff * (int32_t)f) >> 16;
//    return (q15_t)((int32_t)a + scaled);
//
//  diff is int17-range (b-a of two q15), f is unsigned 16-bit, so diff*f fits
//  in int32 without the overflow the cubic has. Signed >> matches the C.
//
//  Combinational. Public domain (CC0).
// ============================================================

`default_nettype none

module hx_lerp (
    input  wire signed [15:0] a,
    input  wire signed [15:0] b,
    input  wire        [31:0] frac_q32,
    output wire signed [15:0] out
);
    wire        [15:0] f    = frac_q32[31:16];         // q16 fractional
    wire signed [31:0] diff = $signed(b) - $signed(a); // b - a, exact
    // diff (signed) * f (unsigned): zero-extend f to keep it unsigned, product
    // is signed. Truncate to 32 bits then arithmetic >>16, exactly as the C.
    wire signed [31:0] prod   = diff * $signed({1'b0, f});
    wire signed [31:0] scaled = prod >>> 16;
    // C returns (q15_t)((int32_t)a + scaled): 32-bit add, truncate to int16.
    wire signed [31:0] sum = $signed(a) + scaled;
    assign out = sum[15:0];
endmodule

`default_nettype wire
