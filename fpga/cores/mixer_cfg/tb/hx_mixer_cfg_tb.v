// hx_mixer_cfg_tb.v — co-sim of hx_mixer_cfg.v against the golden model.
// Drives STM32 register writes and checks each emitted mixer cfg strobe
// (we/ch/field/data) against golden.hex. Writes MUST match gen_vectors.c.
`timescale 1ns/1ps
`default_nettype none

module hx_mixer_cfg_tb;
    localparam CHW = 3;
    reg clk=0, rst=1;
    always #5 clk=~clk;

    reg        reg_we=0; reg [7:0] reg_addr=0; reg [31:0] reg_data=0;
    wire       cfg_we;
    wire [CHW-1:0] cfg_ch;
    wire [2:0] cfg_field;
    wire [31:0] cfg_data;

    hx_mixer_cfg #(.CHW(CHW)) dut (
        .clk(clk), .rst(rst),
        .reg_we(reg_we), .reg_addr(reg_addr), .reg_data(reg_data),
        .cfg_we(cfg_we), .cfg_ch(cfg_ch), .cfg_field(cfg_field), .cfg_data(cfg_data)
    );

    reg [39:0] golden [0:63];
    reg [7:0]  addrs  [0:63];
    reg [31:0] datas  [0:63];
    integer i, n, errors;
    reg        we_g; reg [2:0] ch_g, field_g; reg [31:0] data_g;

    initial begin
        addrs[0]=8'h0A; datas[0]=32'h00000002;
        addrs[1]=8'h0B; datas[1]=32'h00004000;
        addrs[2]=8'h0C; datas[2]=32'h00007FFF;
        addrs[3]=8'h0D; datas[3]=32'h00003000;
        addrs[4]=8'h02; datas[4]=32'h00000002;
        addrs[5]=8'h03; datas[5]=32'h00007FFF;
        addrs[6]=8'h3B; datas[6]=32'h00001234;
        addrs[7]=8'h07; datas[7]=32'hDEADBEEF;
        n=8;
    end

    task do_write(input [7:0] a, input [31:0] d); begin
        @(posedge clk); reg_we<=1; reg_addr<=a; reg_data<=d;
        @(posedge clk); reg_we<=0;      // registered outputs valid after this edge
    end endtask

    initial begin
        $readmemh("golden.hex", golden);
        errors = 0;
        repeat (4) @(posedge clk); rst<=0;
        repeat (2) @(posedge clk);

        for (i=0; i<n; i=i+1) begin
            do_write(addrs[i], datas[i]);
            #1;
            we_g    = golden[i][38];
            ch_g    = golden[i][37:35];
            field_g = golden[i][34:32];
            data_g  = golden[i][31:0];
            if (cfg_we!==we_g || cfg_ch!==ch_g || cfg_field!==field_g || cfg_data!==data_g) begin
                $display("  write %0d: rtl we=%b ch=%0d fld=%0d data=%08x | golden we=%b ch=%0d fld=%0d data=%08x",
                         i, cfg_we,cfg_ch,cfg_field,cfg_data, we_g,ch_g,field_g,data_g);
                errors = errors + 1;
            end
        end

        if (errors==0) $display("COSIM PASS: mixer_cfg decode matches the golden model (%0d writes)", n);
        else           $display("COSIM FAIL: %0d mismatches", errors);
        $finish;
    end
endmodule

`default_nettype wire
