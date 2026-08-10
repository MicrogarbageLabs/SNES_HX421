// hx_actor_tb.v — co-sim of hx_actor.v against the golden model (gen_vectors.c).
// Loads the same 24 actors, runs two cases (lock, rotation), and diffs the OAM
// low-table writes against the golden. See gen_vectors.c.
`timescale 1ns/1ps
`default_nettype none

module hx_actor_tb;
    localparam NACT = 24, N = 128;
    reg clk = 0, rst = 1;
    always #5 clk = ~clk;

    reg        cfg_we=0; reg [2:0] cfg_field=0; reg [31:0] cfg_data=0;
    reg        act_we=0; reg [6:0] act_idx=0; reg [63:0] act_data=0;
    reg        go=0;
    wire       busy, oam_we;
    wire [9:0] oam_addr;
    wire [7:0] oam_data;

    hx_actor dut (
        .clk(clk), .rst(rst),
        .cfg_we(cfg_we), .cfg_field(cfg_field), .cfg_data(cfg_data),
        .act_we(act_we), .act_idx(act_idx), .act_data(act_data),
        .go(go), .busy(busy),
        .oam_we(oam_we), .oam_addr(oam_addr), .oam_data(oam_data)
    );

    // ---- capture OAM bytes + written mask ----
    reg [7:0] oamb [0:511];
    reg       oamw [0:511];
    always @(posedge clk) if (oam_we && oam_addr < 512) begin
        oamb[oam_addr] <= oam_data; oamw[oam_addr] <= 1'b1;
    end

    reg [63:0] actmem [0:NACT-1];
    reg [31:0] golden [0:N-1];
    integer i, k, errors, total_err;

    task cfgw(input [2:0] f, input [31:0] d); begin
        @(posedge clk); cfg_we<=1; cfg_field<=f; cfg_data<=d;
        @(posedge clk); cfg_we<=0;
    end endtask

    task clear_oam; begin for (k=0;k<512;k=k+1) begin oamw[k]=0; oamb[k]=0; end end endtask

    // run one frame and diff against `gfile`
    task run_case(input [2:0] nlock_field_unused, input integer n_lock, input integer phase,
                  input [8*32:1] gfile);
        integer o; reg [31:0] g; reg [7:0] e0,e1,e2,e3; integer errs;
    begin
        clear_oam;
        cfgw(3'd0, 32'd0);           // F_CAMX = 0
        cfgw(3'd1, 32'd0);           // F_CAMY = 0
        cfgw(3'd2, NACT);            // F_COUNT
        cfgw(3'd3, phase);           // F_PHASE
        cfgw(3'd4, 32'd0);           // F_OAMB (base 0)
        cfgw(3'd5, n_lock);          // F_NLOCK
        @(posedge clk); go<=1;
        @(posedge clk); go<=0;
        i=0; while(!busy && i<100)    begin @(posedge clk); i=i+1; end
        i=0; while( busy && i<200000) begin @(posedge clk); i=i+1; end
        repeat (4) @(posedge clk);

        $readmemh(gfile, golden);
        errs = 0;
        for (o=0;o<N;o=o+1) begin
            g = golden[o];
            if (g === 32'hFFFFFFFF) begin
                if (oamw[o*4] || oamw[o*4+1] || oamw[o*4+2] || oamw[o*4+3]) begin
                    if (errs<6) $display("  slot %0d written but golden says untouched", o);
                    errs=errs+1;
                end
            end else begin
                e0=g[7:0]; e1=g[15:8]; e2=g[23:16]; e3=g[31:24];
                if (!oamw[o*4] || oamb[o*4]!==e0 || oamb[o*4+1]!==e1
                    || oamb[o*4+2]!==e2 || oamb[o*4+3]!==e3) begin
                    if (errs<6) $display("  slot %0d rtl=%02x%02x%02x%02x golden=%02x%02x%02x%02x",
                        o, oamb[o*4+3],oamb[o*4+2],oamb[o*4+1],oamb[o*4], e3,e2,e1,e0);
                    errs=errs+1;
                end
            end
        end
        total_err = total_err + errs;
        if (errs==0) $display("  case OK");
        else         $display("  case FAIL: %0d slot mismatches", errs);
    end endtask

    initial begin
        $readmemh("actors.hex", actmem);
        clear_oam;
        total_err = 0;
        repeat (4) @(posedge clk); rst<=0;

        // load the actor list
        for (k=0;k<NACT;k=k+1) begin
            @(posedge clk); act_we<=1; act_idx<=k[6:0]; act_data<=actmem[k];
        end
        @(posedge clk); act_we<=0;

        $display("CASE A (n_lock=2, phase=0):");   run_case(0, 2, 0, "golden_a.hex");
        $display("CASE B (n_lock=0, phase=3):");   run_case(0, 0, 3, "golden_b.hex");

        if (total_err==0) $display("COSIM PASS: actor OAM matches the golden model (both cases)");
        else              $display("COSIM FAIL: %0d total mismatches", total_err);
        $finish;
    end
endmodule

`default_nettype wire
