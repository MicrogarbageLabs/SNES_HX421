// ============================================================
//  tb_cubic.v — co-simulation testbench for hx_cubic.v
//
//  Replays gen_cubic_vectors.c's { taps frac : goldened } lines through the
//  DUT and flags any mismatch. A pass here means the RTL reproduces the C
//  mixer's cubic interpolation bit-for-bit across the whole vector set,
//  including the extreme-tap overflow cases.
//
//  Public domain (CC0). No warranty.
// ============================================================

`timescale 1ns/1ps
`default_nettype none

module tb_cubic;

    reg  signed [15:0] p0, p1, p2, p3;
    reg         [31:0] frac;
    wire signed [15:0] dut_out;

    reg  signed [15:0] golden;

    hx_cubic dut (
        .p0(p0), .p1(p1), .p2(p2), .p3(p3),
        .frac_q32(frac), .out(dut_out)
    );

    integer fd, r, n, fails;
    reg [15:0] p0u, p1u, p2u, p3u, eu;

    initial begin
        fd = $fopen("cubic_vectors.txt", "r");
        if (fd == 0) begin
            $display("FATAL: cannot open cubic_vectors.txt");
            $finish;
        end
        n = 0; fails = 0;

        while (!$feof(fd)) begin
            r = $fscanf(fd, "%h %h %h %h %h %h\n", p0u, p1u, p2u, p3u, frac, eu);
            if (r == 6) begin
                p0 = p0u; p1 = p1u; p2 = p2u; p3 = p3u; golden = eu;
                #1;   // let the combinational DUT settle
                if (dut_out !== golden) begin
                    fails = fails + 1;
                    if (fails <= 20)
                        $display("MISMATCH line %0d: taps %0d %0d %0d %0d frac %08x -> dut %0d golden %0d",
                                 n, p0, p1, p2, p3, frac, dut_out, golden);
                end
                n = n + 1;
            end
        end
        $fclose(fd);

        $display("");
        $display("hx_cubic co-sim: %0d vectors, %0d mismatches", n, fails);
        if (fails == 0) $display("RESULT: PASS - bit-exact to interp_cubic_q15");
        else            $display("RESULT: FAIL");
        $finish;
    end

endmodule

`default_nettype wire
