// ============================================================
//  hx_finalize.v — output finalize stage, bit-exact to finalize_output
//
//  Golden model (engine/audio/audio_mixer.c):
//    int32_t v = accum >> headroom_bits;   // remove headroom
//    if (v >  32767) v =  32767;           // saturate to q15
//    if (v < -32768) v = -32768;
//    v = (v >> out_shift) + out_offset;    // reduce to output resolution
//    if (v > out_max) v = out_max;         // clamp to format range
//    if (v < out_min) v = out_min;
//    return v;
//
//  Kept general (out_shift / offset / min / max are inputs) so it covers any
//  output format. The hardware DAC is 16-bit signed: shift 0, offset 0,
//  min -32768, max 32767 — which reduces this to headroom-shift + saturate.
//  Testing it generally means the co-sim proves every format, not just ours.
//
//  Signed arithmetic shifts throughout, matching C's signed >>. Combinational.
//  Public domain (CC0).
// ============================================================

`default_nettype none

module hx_finalize (
    input  wire signed [31:0] accum,
    input  wire        [3:0]  headroom_bits,   // 0..6 typical
    input  wire        [3:0]  out_shift,       // 15 - bits, 0..15
    input  wire signed [31:0] out_offset,
    input  wire signed [31:0] out_min,
    input  wire signed [31:0] out_max,
    output wire signed [31:0] out
);
    wire signed [31:0] v0 = accum >>> headroom_bits;

    wire signed [31:0] v1 = (v0 >  32767) ? 32'sd32767
                          : (v0 < -32768) ? -32'sd32768
                          : v0;

    wire signed [31:0] v2 = (v1 >>> out_shift) + out_offset;

    assign out = (v2 > out_max) ? out_max
               : (v2 < out_min) ? out_min
               : v2;
endmodule

`default_nettype wire
