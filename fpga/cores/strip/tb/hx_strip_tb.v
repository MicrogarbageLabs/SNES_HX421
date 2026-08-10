// hx_strip_tb.v — co-simulation of hx_strip against runtime/hx421_metatile.c.
// Loads the same map+defs into a PSRAM model, drives one reseed, and diffs the
// staging writes against the C reference's golden output. See gen_vectors.c.
`timescale 1ns/1ps
`default_nettype none

module hx_strip_tb;
    reg clk = 0, rst = 1;
    always #5 clk = ~clk;   // 100 MHz

    // ---- config drive ----
    reg        cfg_we = 0;
    reg [1:0]  cfg_layer = 0;
    reg [4:0]  cfg_field = 0;
    reg [31:0] cfg_data = 0;
    reg        go = 0;
    reg [1:0]  go_layer = 0;
    reg signed [15:0] go_x = 0, go_y = 0;

    // ---- DUT nets ----
    wire        busy, rd_req, strip_we;
    wire [31:0] rd_addr;
    wire [15:0] strip_addr, strip_data;
    reg         rd_ack = 0;
    reg  [15:0] rd_data = 0;

    hx_strip dut (
        .clk(clk), .rst(rst),
        .cfg_we(cfg_we), .cfg_layer(cfg_layer), .cfg_field(cfg_field), .cfg_data(cfg_data),
        .go(go), .go_layer(go_layer), .go_x(go_x), .go_y(go_y), .busy(busy),
        .rd_req(rd_req), .rd_addr(rd_addr), .rd_ack(rd_ack), .rd_data(rd_data),
        .strip_we(strip_we), .strip_addr(strip_addr), .strip_data(strip_data)
    );

    // ---- PSRAM model: 1-cycle latency, combinational read of the loaded image ----
    reg [15:0] psram [0:8191];
    always @(posedge clk) begin
        if (rst) rd_ack <= 1'b0;
        else     rd_ack <= rd_req & ~rd_ack;      // pulse ack one cycle after req
    end
    always @(*) rd_data = psram[rd_addr[12:0]];

    // ---- capture staging writes + golden ----
    reg [15:0] result [0:2047];
    reg [15:0] golden [0:2047];
    integer i, errors;
    reg [15:0] hit [0:2047];   // 1 = written

    integer nwrite = 0, nreq = 0;
    always @(posedge clk) if (strip_we && strip_addr < 2048) begin
        result[strip_addr] <= strip_data;
        hit[strip_addr] <= 16'd1;
        nwrite = nwrite + 1;
    end
    always @(posedge clk) if (rd_req && !rd_ack) nreq = nreq + 1;
    reg busy_seen = 0;
    always @(posedge clk) if (busy) busy_seen <= 1;

    // ---- config write helper ----
    task cfgw(input [4:0] f, input [31:0] d); begin
        @(posedge clk); cfg_we<=1; cfg_field<=f; cfg_data<=d; cfg_layer<=0;
        @(posedge clk); cfg_we<=0;
    end endtask

    initial begin
        $readmemh("psram.hex", psram);
        $readmemh("golden.hex", golden);
        for (i=0;i<2048;i=i+1) hit[i]=0;

        repeat (4) @(posedge clk); rst<=0;

        // layer 0 config: map@0, cols@0, defs@0x1000, w=32,h=32,defc=16,
        // flags = {oob=FFFF, clamp=0, wrap=0, shift=1}, strip_base=0
        cfgw(5'd0, 32'd0);          // F_MAP
        cfgw(5'd1, 32'd0);          // F_COLS
        cfgw(5'd2, 32'h0000_1000);  // F_DEFS
        cfgw(5'd3, 32'd32);         // F_W
        cfgw(5'd4, 32'd32);         // F_H
        cfgw(5'd5, 32'd16);         // F_DEFCNT
        cfgw(5'd6, 32'hFFFF_0001);  // F_FLAGS: oob=FFFF, shift=1
        cfgw(5'd7, 32'd0);          // F_STRIP

        // reseed: first go with camera (128,16) -> want_l=0, want_t=0
        @(posedge clk); go_layer<=0; go_x<=16'sd128; go_y<=16'sd16; go<=1;
        @(posedge clk); go<=0;

        // wait for busy to ASSERT, then for it to deassert (avoids the propagation race)
        i = 0; while (!busy && i < 100)    begin @(posedge clk); i=i+1; end
        i = 0; while ( busy && i < 500000) begin @(posedge clk); i=i+1; end
        repeat (4) @(posedge clk);
        $display("DBG busy_seen=%0d  strip_writes=%0d  psram_reqs=%0d  waited=%0d cycles",
                 busy_seen, nwrite, nreq, i);

        // ---- diff ----
        errors = 0;
        for (i=0;i<2048;i=i+1) begin
            if (!hit[i]) begin
                if (errors < 8) $display("MISS  idx %0d never written", i);
                errors = errors + 1;
            end else if (result[i] !== golden[i]) begin
                if (errors < 8) $display("DIFF  idx %0d  rtl=%04x  golden=%04x", i, result[i], golden[i]);
                errors = errors + 1;
            end
        end

        if (errors==0) $display("COSIM PASS: 2048/2048 entries match the C reference");
        else           $display("COSIM FAIL: %0d mismatches", errors);
        $finish;
    end
endmodule

`default_nettype wire
