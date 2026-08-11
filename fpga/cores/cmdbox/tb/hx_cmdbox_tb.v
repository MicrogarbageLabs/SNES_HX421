// hx_cmdbox_tb.v — co-sim of hx_cmdbox.v against the golden model (gen_vectors.c).
// Drives the mailbox as the SNES + STM32 would: byte-write a command block, ring
// the doorbell, read the block back, ack; then reuse + a doorbell/ack collision.
// Diffs each CHECK point against golden.hex.
`timescale 1ns/1ps
`default_nettype none

module hx_cmdbox_tb;
    reg clk=0, rst=1;
    always #5 clk=~clk;

    reg        w_we=0; reg [7:0] w_addr=0, w_data=0;
    reg [7:0]  r_addr=0;
    wire [7:0] r_data;
    reg        ack=0;
    wire       pending;

    hx_cmdbox dut (
        .clk(clk), .rst(rst),
        .w_we(w_we), .w_addr(w_addr), .w_data(w_data),
        .r_addr(r_addr), .r_data(r_data),
        .pending(pending), .ack(ack)
    );

    reg [7:0] golden [0:63];
    integer gi, errors;

    task wr(input [7:0] a, input [7:0] d); begin
        @(posedge clk); w_we<=1; w_addr<=a; w_data<=d;
        @(posedge clk); w_we<=0;
    end endtask

    task ack_pulse; begin
        @(posedge clk); ack<=1;
        @(posedge clk); ack<=0;
    end endtask

    // doorbell write and ack asserted in the SAME cycle (collision test)
    task wr_and_ack(input [7:0] a, input [7:0] d); begin
        @(posedge clk); w_we<=1; w_addr<=a; w_data<=d; ack<=1;
        @(posedge clk); w_we<=0; ack<=0;
    end endtask

    task chk(input [7:0] val); begin
        if (val !== golden[gi]) begin
            $display("  check %0d: rtl=%02x golden=%02x", gi, val, golden[gi]);
            errors = errors + 1;
        end
        gi = gi + 1;
    end endtask

    task chk_pending; begin #1; chk({7'd0, pending}); end endtask
    task chk_read(input [7:0] a); begin r_addr = a; #1; chk(r_data); end endtask

    initial begin
        $readmemh("golden.hex", golden);
        errors = 0; gi = 0;
        repeat (4) @(posedge clk); rst<=0;
        repeat (2) @(posedge clk);

        // Command A: block at 0..11 then doorbell
        wr(8'd0,8'h01); wr(8'd1,8'h00); wr(8'd2,8'h0A); wr(8'd3,8'h00);
        wr(8'd4,8'h01); wr(8'd5,8'h02); wr(8'd6,8'hFF); wr(8'd7,8'h80);
        wr(8'd8,8'h00); wr(8'd9,8'h00); wr(8'd10,8'h00); wr(8'd11,8'h00);
        wr(8'hFF,8'h01);
        chk_pending;                                   // 1
        chk_read(8'd0);  chk_read(8'd1);  chk_read(8'd2);  chk_read(8'd3);
        chk_read(8'd4);  chk_read(8'd5);  chk_read(8'd6);  chk_read(8'd7);
        chk_read(8'd8);  chk_read(8'd9);  chk_read(8'd10); chk_read(8'd11); // 2..13
        chk_read(8'hFF);                               // 14
        ack_pulse; chk_pending;                        // 15

        // Command B: overwrite + re-ring
        wr(8'd0,8'h04); wr(8'd1,8'h01); wr(8'd6,8'h40); wr(8'd7,8'hC8);
        wr(8'hFF,8'h02);
        chk_pending;                                   // 16
        chk_read(8'd0); chk_read(8'd6); chk_read(8'd7); chk_read(8'hFF); // 17..20

        // collision: doorbell + ack same cycle -> pending stays 1
        wr_and_ack(8'hFF,8'h03); chk_pending;          // 21
        ack_pulse; chk_pending;                        // 22

        if (errors==0) $display("COSIM PASS: cmdbox mailbox matches the golden model (%0d checks)", gi);
        else           $display("COSIM FAIL: %0d mismatches", errors);
        $finish;
    end
endmodule

`default_nettype wire
