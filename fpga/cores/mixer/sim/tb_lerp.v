`timescale 1ns/1ps
`default_nettype none
module tb_lerp;
    reg  signed [15:0] a, b;
    reg         [31:0] frac;
    wire signed [15:0] dut_out;
    reg  signed [15:0] golden;

    hx_lerp dut (.a(a), .b(b), .frac_q32(frac), .out(dut_out));

    integer fd, r, n, fails;
    reg [15:0] au, bu, eu;
    initial begin
        fd = $fopen("lerp_vectors.txt", "r");
        if (fd == 0) begin $display("FATAL: no lerp_vectors.txt"); $finish; end
        n = 0; fails = 0;
        while (!$feof(fd)) begin
            r = $fscanf(fd, "%h %h %h %h\n", au, bu, frac, eu);
            if (r == 4) begin
                a = au; b = bu; golden = eu;
                #1;
                if (dut_out !== golden) begin
                    fails = fails + 1;
                    if (fails <= 20) $display("MISMATCH %0d: a=%0d b=%0d frac=%08x dut=%0d exp=%0d",
                                              n, a, b, frac, dut_out, golden);
                end
                n = n + 1;
            end
        end
        $fclose(fd);
        $display("hx_lerp co-sim: %0d vectors, %0d mismatches", n, fails);
        if (fails == 0) $display("RESULT: PASS - bit-exact to interp_linear_q15"); else $display("RESULT: FAIL");
        $finish;
    end
endmodule
`default_nettype wire
