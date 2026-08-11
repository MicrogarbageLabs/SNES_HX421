// hx_mixer_cfg_tb.v — co-sim of the byte-assembling hx_mixer_cfg.v.
// Drives FA-style byte register writes (index={ch,field,bytesel}, value) and
// checks: cfg_we fires ONLY on a field's last byte, with the right ch/field and
// the assembled cfg_data. Writes MUST match gen_vectors.c.
`timescale 1ns/1ps
`default_nettype none

module hx_mixer_cfg_tb;
    localparam CHW = 3;
    reg clk=0, rst=1;
    always #5 clk=~clk;

    reg        reg_we=0; reg [7:0] reg_index=0, reg_value=0;
    wire       cfg_we;
    wire [CHW-1:0] cfg_ch;
    wire [2:0] cfg_field;
    wire [31:0] cfg_data;

    hx_mixer_cfg #(.CHW(CHW)) dut (
        .clk(clk), .rst(rst),
        .reg_we(reg_we), .reg_index(reg_index), .reg_value(reg_value),
        .cfg_we(cfg_we), .cfg_ch(cfg_ch), .cfg_field(cfg_field), .cfg_data(cfg_data)
    );

    reg [39:0] golden [0:63];
    reg [7:0]  idxs   [0:63];
    reg [7:0]  vals   [0:63];
    integer i, n, errors;
    reg        we_g; reg [2:0] ch_g, field_g; reg [31:0] data_g;

    initial begin
        idxs[0]=8'h2C; vals[0]=8'h00;  idxs[1]=8'h2D; vals[1]=8'h40;
        idxs[2]=8'h30; vals[2]=8'hFF;  idxs[3]=8'h31; vals[3]=8'h7F;
        idxs[4]=8'h08; vals[4]=8'h02;
        idxs[5]=8'h00; vals[5]=8'h44;  idxs[6]=8'h01; vals[6]=8'h33;
        idxs[7]=8'h02; vals[7]=8'h22;  idxs[8]=8'h03; vals[8]=8'h11;
        idxs[9]=8'h58; vals[9]=8'h00;  idxs[10]=8'h59; vals[10]=8'h02; idxs[11]=8'h5A; vals[11]=8'h00;
        n=12;
    end

    task do_write(input [7:0] idx, input [7:0] d); begin
        @(posedge clk); reg_we<=1; reg_index<=idx; reg_value<=d;
        @(posedge clk); reg_we<=0;      // registered outputs valid after this edge
    end endtask

    initial begin
        $readmemh("golden.hex", golden);
        errors = 0;
        repeat (4) @(posedge clk); rst<=0;
        repeat (2) @(posedge clk);

        for (i=0; i<n; i=i+1) begin
            do_write(idxs[i], vals[i]);
            #1;
            we_g    = golden[i][38];
            ch_g    = golden[i][37:35];
            field_g = golden[i][34:32];
            data_g  = golden[i][31:0];
            if (cfg_we !== we_g) begin
                $display("  write %0d: cfg_we rtl=%b golden=%b", i, cfg_we, we_g);
                errors = errors + 1;
            end else if (we_g && (cfg_ch!==ch_g || cfg_field!==field_g || cfg_data!==data_g)) begin
                $display("  write %0d: commit rtl ch=%0d fld=%0d data=%08x | golden ch=%0d fld=%0d data=%08x",
                         i, cfg_ch,cfg_field,cfg_data, ch_g,field_g,data_g);
                errors = errors + 1;
            end
        end

        if (errors==0) $display("COSIM PASS: mixer_cfg byte-assembler matches the golden model (%0d writes)", n);
        else           $display("COSIM FAIL: %0d mismatches", errors);
        $finish;
    end
endmodule

`default_nettype wire
