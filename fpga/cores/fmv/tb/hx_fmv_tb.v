// hx_fmv_tb.v — co-sim of hx_fmv.v against the golden model (gen_vectors.c).
// Loads a frame image into a PSRAM model, runs two sub-frame stagings, and diffs
// the staging writes + the two DMA descriptors against the golden. See gen_vectors.c.
`timescale 1ns/1ps
`default_nettype none

module hx_fmv_tb;
    localparam CHR_PS=64, TMAP_PS=16, STAGE_N=CHR_PS+TMAP_PS;
    reg clk=0, rst=1;
    always #5 clk=~clk;

    reg        cfg_we=0; reg [3:0] cfg_field=0; reg [31:0] cfg_data=0;
    reg        go=0; reg [2:0] go_sub=0; reg go_parity=0;
    wire       busy, rd_req, st_we, desc_we;
    wire [31:0] rd_addr;
    wire [15:0] st_addr, st_data, desc_src, desc_size, desc_vdst;
    wire [7:0]  desc_vmain;
    reg         rd_ack=0; reg [15:0] rd_data=0;

    hx_fmv dut (
        .clk(clk), .rst(rst),
        .cfg_we(cfg_we), .cfg_field(cfg_field), .cfg_data(cfg_data),
        .go(go), .go_sub(go_sub), .go_parity(go_parity), .busy(busy),
        .rd_req(rd_req), .rd_addr(rd_addr), .rd_ack(rd_ack), .rd_data(rd_data),
        .st_we(st_we), .st_addr(st_addr), .st_data(st_data),
        .desc_we(desc_we), .desc_src(desc_src), .desc_size(desc_size),
        .desc_vdst(desc_vdst), .desc_vmain(desc_vmain)
    );

    // PSRAM model
    reg [15:0] psram [0:4095];
    always @(posedge clk) rd_ack <= rst ? 1'b0 : (rd_req & ~rd_ack);
    always @(*) rd_data = psram[rd_addr[11:0]];

    // capture staging + descriptors
    reg [15:0] stg [0:STAGE_N-1];
    reg        stw [0:STAGE_N-1];
    reg [63:0] dcap [0:3]; integer ndesc;
    always @(posedge clk) if (st_we && st_addr < STAGE_N) begin
        stg[st_addr] <= st_data; stw[st_addr] <= 1'b1;
    end
    always @(posedge clk) if (desc_we && ndesc < 4) begin
        dcap[ndesc] <= {8'd0, desc_vmain, desc_vdst, desc_size, desc_src};
        ndesc <= ndesc + 1;
    end

    reg [15:0] gstage [0:STAGE_N-1];
    reg [63:0] gdesc  [0:1];
    integer i, k, total_err;

    task cfgw(input [3:0] f, input [31:0] d); begin
        @(posedge clk); cfg_we<=1; cfg_field<=f; cfg_data<=d;
        @(posedge clk); cfg_we<=0;
    end endtask

    task run_case(input integer idx, input integer sub, input integer parity,
                  input [8*20:1] sfile, input [8*20:1] dfile);
        integer errs;
    begin
        for (k=0;k<STAGE_N;k=k+1) begin stw[k]=0; stg[k]=0; end
        ndesc = 0;
        @(posedge clk); go_sub<=sub[2:0]; go_parity<=parity[0]; go<=1;
        @(posedge clk); go<=0;
        i=0; while(!busy && i<100)   begin @(posedge clk); i=i+1; end
        i=0; while( busy && i<100000) begin @(posedge clk); i=i+1; end
        repeat (4) @(posedge clk);

        $readmemh(sfile, gstage);
        $readmemh(dfile, gdesc);
        errs = 0;
        for (i=0;i<STAGE_N;i=i+1)
            if (!stw[i] || stg[i]!==gstage[i]) begin
                if (errs<6) $display("  stage[%0d] rtl=%04x golden=%04x wr=%0b", i, stg[i], gstage[i], stw[i]);
                errs=errs+1;
            end
        if (ndesc !== 2) begin $display("  desc count = %0d (expected 2)", ndesc); errs=errs+1; end
        for (i=0;i<2;i=i+1)
            if (dcap[i]!==gdesc[i]) begin
                $display("  desc[%0d] rtl=%016h golden=%016h", i, dcap[i], gdesc[i]);
                errs=errs+1;
            end
        total_err = total_err + errs;
        if (errs==0) $display("  case OK"); else $display("  case FAIL");
    end endtask

    initial begin
        $readmemh("psram.hex", psram);
        total_err = 0;
        repeat (4) @(posedge clk); rst<=0;
        // config: chr_base=0x100, tmap_base=0x800, chr_ps=64, tmap_ps=16,
        //         stage=0, vram_chr=0x2000, vram_ovl=0x1000, vram_tmap=0
        cfgw(4'd0, 32'h0000_0100);   // F_CHRBASE
        cfgw(4'd1, 32'h0000_0800);   // F_TMAPBASE
        cfgw(4'd2, 32'd64);          // F_CHRPS
        cfgw(4'd3, 32'd16);          // F_TMAPPS
        cfgw(4'd4, 32'd0);           // F_STAGE
        cfgw(4'd5, 32'h0000_2000);   // F_VCHR
        cfgw(4'd6, 32'h0000_1000);   // F_VOVL
        cfgw(4'd7, 32'd0);           // F_VTMAP

        $display("CASE 0 (sub=0, parity=0):"); run_case(0,0,0,"golden_stage_0.hex","golden_desc_0.hex");
        $display("CASE 1 (sub=1, parity=1):"); run_case(1,1,1,"golden_stage_1.hex","golden_desc_1.hex");

        if (total_err==0) $display("COSIM PASS: FMV staging + descriptors match the golden model");
        else $display("COSIM FAIL");
        $finish;
    end
endmodule

`default_nettype wire
