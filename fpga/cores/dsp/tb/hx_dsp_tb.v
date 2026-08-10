// hx_dsp_tb.v — co-sim of hx_dsp.v against the golden model (gen_vectors.c).
// Drives the mailbox coprocessor exactly as the 65816 would: byte-write FUNC +
// operands, START, wait for ready, read back 4 result bytes; diffs each op.
`timescale 1ns/1ps
`default_nettype none

module hx_dsp_tb;
    reg clk=0, rst=1;
    always #5 clk=~clk;

    reg        w_we=0; reg [3:0] w_addr=0; reg [7:0] w_data=0;
    reg [2:0]  r_addr=0;
    wire [7:0] r_data;
    wire       ready;

    hx_dsp dut (
        .clk(clk), .rst(rst),
        .w_we(w_we), .w_addr(w_addr), .w_data(w_data),
        .r_addr(r_addr), .r_data(r_data), .ready(ready)
    );

    reg [31:0] golden [0:31];
    integer i, opn, errors;
    reg [31:0] res;

    task wr(input [3:0] a, input [7:0] d); begin
        @(posedge clk); w_we<=1; w_addr<=a; w_data<=d;
        @(posedge clk); w_we<=0;
    end endtask

    // one op: write func + args, START, wait ready, read result bytes
    task run_op(input [7:0] func, input [31:0] a, input [31:0] b);
        integer w;
    begin
        wr(4'd0, func);
        wr(4'd1, a[7:0]);  wr(4'd2, a[15:8]);  wr(4'd3, a[23:16]);  wr(4'd4, a[31:24]);
        wr(4'd5, b[7:0]);  wr(4'd6, b[15:8]);  wr(4'd7, b[23:16]);  wr(4'd8, b[31:24]);
        wr(4'd15, 8'd0);                    // START
        // wait for busy to ASSERT (ready low), then for completion — avoids the
        // propagation race; models the 65816's fixed NOP pad + result read.
        w=0; while( ready && w<10)  begin @(posedge clk); w=w+1; end
        w=0; while(!ready && w<200) begin @(posedge clk); w=w+1; end
        // read 4 result bytes by index
        @(posedge clk); r_addr<=0; @(posedge clk); res[7:0]=r_data;
        @(posedge clk); r_addr<=1; @(posedge clk); res[15:8]=r_data;
        @(posedge clk); r_addr<=2; @(posedge clk); res[23:16]=r_data;
        @(posedge clk); r_addr<=3; @(posedge clk); res[31:24]=r_data;

        if (res !== golden[opn]) begin
            $display("  op %0d func=%0d a=%08x b=%08x  rtl=%08x golden=%08x",
                     opn, func, a, b, res, golden[opn]);
            errors = errors + 1;
        end
        opn = opn + 1;
    end endtask

    initial begin
        $readmemh("golden.hex", golden);
        errors = 0; opn = 0;
        repeat (4) @(posedge clk); rst<=0;
        repeat (2) @(posedge clk);

        run_op(8'd0, 32'd300, -32'sd7);                 // MUL 300*-7
        run_op(8'd3, 32'd4, 32'd5);                     // MACINIT 4*5 = 20
        run_op(8'd1, 32'd6, 32'd7);                     // MAC +42 = 62
        run_op(8'd1, 32'd2, 32'd3);                     // MAC +6  = 68
        run_op(8'd2, 32'd100000, 32'd7);                // DIV 100000/7
        run_op(8'd2, 32'd12345, 32'd0);                 // DIV by zero
        run_op(8'd0, -32'sd32768, -32'sd32768);         // MUL -32768*-32768
        run_op(8'd4, 32'd0,   32'd0);                   // SIN 0deg
        run_op(8'd5, 32'd0,   32'd0);                   // COS 0deg
        run_op(8'd4, 32'd256, 32'd0);                   // SIN 90deg
        run_op(8'd4, 32'd512, 32'd0);                   // SIN 180deg
        run_op(8'd4, 32'd768, 32'd0);                   // SIN 270deg
        run_op(8'd4, 32'd128, 32'd0);                   // SIN 45deg
        run_op(8'd5, 32'd128, 32'd0);                   // COS 45deg

        if (errors==0) $display("COSIM PASS: DSP results match the golden model (%0d ops)", opn);
        else           $display("COSIM FAIL: %0d mismatches", errors);
        $finish;
    end
endmodule

`default_nettype wire
