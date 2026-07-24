`timescale 1ns/1ps
`default_nettype none
module tb_scale;
    reg  signed [15:0] a, b;
    wire signed [15:0] dut_out;
    reg  signed [15:0] golden;

    hx_scale dut (.a(a), .b(b), .out(dut_out));

    integer fd, r, n, fails;
    reg [15:0] au, bu, eu;
    initial begin
        fd = $fopen("scale_vectors.txt", "r");
        if (fd == 0) begin $display("FATAL: no scale_vectors.txt"); $finish; end
        n = 0; fails = 0;
        while (!$feof(fd)) begin
            r = $fscanf(fd, "%h %h %h\n", au, bu, eu);
            if (r == 3) begin
                a = au; b = bu; golden = eu;
                #1;
                if (dut_out !== golden) begin
                    fails = fails + 1;
                    if (fails <= 20) $display("MISMATCH %0d: a=%0d b=%0d dut=%0d exp=%0d",
                                              n, a, b, dut_out, golden);
                end
                n = n + 1;
            end
        end
        $fclose(fd);
        $display("hx_scale co-sim: %0d vectors, %0d mismatches", n, fails);
        if (fails == 0) $display("RESULT: PASS - bit-exact to q15_sat_mul"); else $display("RESULT: FAIL");
        $finish;
    end
endmodule
`default_nettype wire
