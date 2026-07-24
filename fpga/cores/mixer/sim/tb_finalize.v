`timescale 1ns/1ps
`default_nettype none
module tb_finalize;
    reg  signed [31:0] accum;
    reg         [3:0]  headroom, out_shift;
    reg  signed [31:0] out_offset, out_min, out_max;
    wire signed [31:0] dut_out;
    reg  signed [31:0] golden;

    hx_finalize dut (.accum(accum), .headroom_bits(headroom), .out_shift(out_shift),
                     .out_offset(out_offset), .out_min(out_min), .out_max(out_max),
                     .out(dut_out));

    integer fd, r, n, fails;
    reg [31:0] au, ou, mnu, mxu, eu;
    reg [3:0]  hr, sh;
    initial begin
        fd = $fopen("finalize_vectors.txt", "r");
        if (fd == 0) begin $display("FATAL: no finalize_vectors.txt"); $finish; end
        n = 0; fails = 0;
        while (!$feof(fd)) begin
            r = $fscanf(fd, "%h %h %h %h %h %h %h\n", au, hr, sh, ou, mnu, mxu, eu);
            if (r == 7) begin
                accum = au; headroom = hr; out_shift = sh;
                out_offset = ou; out_min = mnu; out_max = mxu; golden = eu;
                #1;
                if (dut_out !== golden) begin
                    fails = fails + 1;
                    if (fails <= 20) $display("MISMATCH %0d: accum=%08x hr=%0d sh=%0d dut=%0d exp=%0d",
                                              n, accum, headroom, out_shift, dut_out, golden);
                end
                n = n + 1;
            end
        end
        $fclose(fd);
        $display("hx_finalize co-sim: %0d vectors, %0d mismatches", n, fails);
        if (fails == 0) $display("RESULT: PASS - bit-exact to finalize_output"); else $display("RESULT: FAIL");
        $finish;
    end
endmodule
`default_nettype wire
