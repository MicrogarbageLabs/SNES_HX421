// ============================================================
//  hx_scale.v — saturating q15 multiply, bit-exact to q15_sat_mul
//
//  Golden model (engine/math/fixed_point.h):
//    q15_sat_mul(a,b) = fx_sat16_( ((int32_t)a * (int32_t)b) >> 15 )
//  where fx_sat16_ clamps to [-32768, 32767].
//
//  This is the mixer's volume and pan-gain application: sample * gain. Both
//  operands q15, product q30, arithmetic >>15 back to q15, then saturate (the
//  only value that overflows is -32768 * -32768 = +32768, which clamps).
//
//  Combinational. Public domain (CC0).
// ============================================================

`default_nettype none

module hx_scale (
    input  wire signed [15:0] a,
    input  wire signed [15:0] b,
    output wire signed [15:0] out
);
    wire signed [31:0] prod = $signed(a) * $signed(b);   // q30
    wire signed [31:0] v    = prod >>> 15;               // back to q15
    assign out = (v >  32767) ? 16'sd32767
               : (v < -32768) ? -16'sd32768
               : v[15:0];
endmodule

`default_nettype wire
