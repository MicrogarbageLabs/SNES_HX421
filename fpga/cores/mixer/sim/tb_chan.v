// ============================================================
//  tb_chan.v — co-sim for hx_chan.v against the real mixer's output sequence
//
//  Reads each CASE from chan_vectors.txt: loads its source samples into a
//  memory, primes and produces N output frames through the DUT, and compares
//  the produced sequence to what mixer_render emitted. Combinational 4-wide
//  source read models a BRAM fetch.
//
//  Public domain (CC0). No warranty.
// ============================================================

`timescale 1ns/1ps
`default_nettype none

module tb_chan;
    reg         clk = 0;
    reg         rst, start, produce, cubic;
    reg  [63:0] step;
    wire [31:0] rd_base;
    wire signed [15:0] rd_s0, rd_s1, rd_s2, rd_s3;
    wire signed [15:0] dut_out;
    wire        valid;

    // source memory + combinational 4-wide read (reads past the end return 0,
    // matching the C's peek-past-end -> silence)
    reg signed [15:0] src [0:4095];
    localparam SMAX = 12'd4095;
    assign rd_s0 = (rd_base       <= SMAX) ? src[rd_base]       : 16'sd0;
    assign rd_s1 = (rd_base + 1   <= SMAX) ? src[rd_base + 1]   : 16'sd0;
    assign rd_s2 = (rd_base + 2   <= SMAX) ? src[rd_base + 2]   : 16'sd0;
    assign rd_s3 = (rd_base + 3   <= SMAX) ? src[rd_base + 3]   : 16'sd0;

    hx_chan dut (
        .clk(clk), .rst(rst), .start(start), .produce(produce),
        .cubic(cubic), .step_q32(step),
        .rd_base(rd_base), .rd_s0(rd_s0), .rd_s1(rd_s1), .rd_s2(rd_s2), .rd_s3(rd_s3),
        .out(dut_out), .valid(valid)
    );

    always #5 clk = ~clk;

    integer fd, r, i, k;
    integer n_case, n_src, n_out, fails, total_out, total_fail;
    reg [31:0] c_cubic, step_hi, step_lo;
    reg [15:0] sv, ev;
    reg signed [15:0] expect_out [0:4095];

    task do_reset;
        begin
            rst = 1; start = 0; produce = 0;
            @(posedge clk); #1; @(posedge clk); #1;
            rst = 0;
        end
    endtask

    initial begin
        fd = $fopen("chan_vectors.txt", "r");
        if (fd == 0) begin $display("FATAL: no chan_vectors.txt"); $finish; end
        n_case = 0; total_out = 0; total_fail = 0;

        while (!$feof(fd)) begin
            // CASE <cubic> <step_hi> <step_lo> <n_src> <n_out>
            r = $fscanf(fd, "CASE %d %h %h %d %d\n", c_cubic, step_hi, step_lo, n_src, n_out);
            if (r != 5) begin
                // skip any stray line
                r = $fscanf(fd, "%h\n", sv);
                if ($feof(fd)) ; // fall through to loop test
            end else begin
                for (i = 0; i < n_src; i = i + 1) begin
                    r = $fscanf(fd, "%h\n", sv); src[i] = sv;
                end
                for (i = 0; i < n_out; i = i + 1) begin
                    r = $fscanf(fd, "%h\n", ev); expect_out[i] = ev;
                end

                cubic = c_cubic[0];
                step  = {step_hi, step_lo};
                do_reset;

                // prime
                start = 1; @(posedge clk); #1; start = 0;

                // produce n_out frames, capturing dut_out on each valid
                fails = 0;
                for (k = 0; k < n_out; k = k + 1) begin
                    produce = 1; @(posedge clk); #1; produce = 0;
                    // dut_out/valid registered on that edge
                    if (dut_out !== expect_out[k]) begin
                        fails = fails + 1;
                        if (total_fail + fails <= 20)
                            $display("MISMATCH case %0d cubic=%0d frame %0d: dut=%0d exp=%0d",
                                     n_case, cubic, k, dut_out, expect_out[k]);
                    end
                    total_out = total_out + 1;
                end
                total_fail = total_fail + fails;
                n_case = n_case + 1;
            end
        end
        $fclose(fd);

        $display("hx_chan co-sim: %0d cases, %0d frames, %0d mismatches",
                 n_case, total_out, total_fail);
        if (total_fail == 0) $display("RESULT: PASS - sequence bit-exact to mixer_render");
        else                 $display("RESULT: FAIL");
        $finish;
    end
endmodule

`default_nettype wire
