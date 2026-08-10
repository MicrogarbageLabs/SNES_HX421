// hx_scene_tb.v — co-sim of hx_scene.v against the golden model (gen_vectors.c).
// Configures the same register bank + descriptors, runs the engine, and diffs the
// emitted 65816 code byte stream against golden.hex. See gen_vectors.c.
`timescale 1ns/1ps
`default_nettype none

module hx_scene_tb;
    reg clk=0, rst=1;
    always #5 clk=~clk;

    reg        reg_we=0; reg [3:0] reg_idx=0; reg reg_is16=0;
    reg [15:0] reg_addr=0, reg_val=0; reg [4:0] reg_count=0;
    reg        desc_we=0; reg [4:0] desc_idx=0;
    reg [15:0] desc_src=0, desc_size=0, desc_vdst=0; reg [7:0] desc_vmain=0;
    reg [5:0]  desc_count=0;
    reg        go=0;
    wire       busy, code_we;
    wire [15:0] code_addr;
    wire [7:0]  code_data;

    hx_scene dut (
        .clk(clk), .rst(rst),
        .reg_we(reg_we), .reg_idx(reg_idx), .reg_is16(reg_is16),
        .reg_addr(reg_addr), .reg_val(reg_val), .reg_count(reg_count),
        .desc_we(desc_we), .desc_idx(desc_idx), .desc_src(desc_src), .desc_size(desc_size),
        .desc_vdst(desc_vdst), .desc_vmain(desc_vmain), .desc_count(desc_count),
        .go(go), .busy(busy),
        .code_we(code_we), .code_addr(code_addr), .code_data(code_data)
    );

    // capture code stream
    reg [7:0] code [0:1023];
    reg       cw   [0:1023];
    always @(posedge clk) if (code_we && code_addr < 1024) begin
        code[code_addr] <= code_data; cw[code_addr] <= 1'b1;
    end

    reg [7:0] golden [0:1023];
    integer i, glen, errors;

    task regw(input [3:0] ix, input is16, input [15:0] a, input [15:0] v); begin
        @(posedge clk); reg_we<=1; reg_idx<=ix; reg_is16<=is16; reg_addr<=a; reg_val<=v;
        @(posedge clk); reg_we<=0;
    end endtask
    task descw(input [4:0] ix, input [15:0] s, input [15:0] sz, input [15:0] vd, input [7:0] vm); begin
        @(posedge clk); desc_we<=1; desc_idx<=ix; desc_src<=s; desc_size<=sz; desc_vdst<=vd; desc_vmain<=vm;
        @(posedge clk); desc_we<=0;
    end endtask

    integer rtl_len;
    initial begin
        for (i=0;i<1024;i=i+1) begin cw[i]=0; code[i]=0; end
        repeat (4) @(posedge clk); rst<=0;

        // register bank (must match gen_vectors.c)
        regw(0, 0, 16'h2105, 16'h0009);
        regw(1, 1, 16'h2116, 16'h1234);
        regw(2, 0, 16'h212C, 16'h0017);
        // descriptors
        descw(0, 16'h0000, 16'h0800, 16'h0000, 8'h80);
        descw(1, 16'h0800, 16'h0100, 16'h4000, 8'h81);
        @(posedge clk); reg_count<=3; desc_count<=2;

        @(posedge clk); go<=1;
        @(posedge clk); go<=0;
        i=0; while(!busy && i<100)   begin @(posedge clk); i=i+1; end
        i=0; while( busy && i<100000) begin @(posedge clk); i=i+1; end
        repeat (4) @(posedge clk);

        // load golden, count lengths from the capture mask / golden
        $readmemh("golden.hex", golden);
        glen = 0;    for (i=0;i<1024;i=i+1) if (golden[i] !== 8'hxx) glen = i+1;
        rtl_len = 0; for (i=0;i<1024;i=i+1) if (cw[i])               rtl_len = i+1;

        errors = 0;
        if (rtl_len !== glen) begin
            $display("  length: rtl=%0d golden=%0d", rtl_len, glen);
            errors = errors + 1;
        end
        for (i=0;i<glen;i=i+1)
            if (!cw[i] || code[i]!==golden[i]) begin
                if (errors<10) $display("  byte %0d rtl=%02x golden=%02x wr=%0b", i, code[i], golden[i], cw[i]);
                errors=errors+1;
            end

        if (errors==0) $display("COSIM PASS: scene DMA body (%0d bytes) matches the golden model", glen);
        else           $display("COSIM FAIL: %0d mismatches", errors);
        $finish;
    end
endmodule

`default_nettype wire
