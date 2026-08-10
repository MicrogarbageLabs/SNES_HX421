// ============================================================
//  hx_fmv.v — FMV prep engine: sub-frame NMI-DMA staging (RTL skeleton)
//
//  Reads a decoded video frame from PSRAM (streamed there by the arbiter from
//  the clip FIFO / preroll head) and stages one SUB-FRAME's worth into the BRAM
//  the SNES NMI DMAs to VRAM, in the double-buffered layout the FMV engine uses
//  (see docs/fmv-engine.md). A video frame is split into n_sub sub-frames
//  (4 @15fps, 3 @20fps), one per SNES NMI; each carries a slice of the CHR and a
//  slice of the tilemap.
//
//  Per go(sub, parity):
//    - copy chr_per_sub words from PSRAM[chr_base + sub*chr_per_sub] -> staging,
//      emit a DMA descriptor to VRAM CHR (double-buffered: parity picks buffer
//      A at vram_chr_base or the overlapping buffer B one granule earlier);
//    - copy tmap_per_sub words from PSRAM[tmap_base + sub*tmap_per_sub] ->
//      staging, emit a descriptor to the VRAM tilemap band.
//
//  This is the fixed-function PSRAM->BRAM stager the FMV plan calls for. Shares
//  the copy shape with the strip engine but is a straight bulk copy (no metatile
//  lookup). RESOURCE-REPRESENTATIVE skeleton for the LE tally. CC0.
// ============================================================

`default_nettype none

module hx_fmv (
    input  wire        clk,
    input  wire        rst,

    // config
    input  wire        cfg_we,
    input  wire [3:0]  cfg_field,   // see localparams
    input  wire [31:0] cfg_data,

    // start one sub-frame
    input  wire        go,
    input  wire [2:0]  go_sub,      // sub-frame index
    input  wire        go_parity,   // double-buffer: 0=buffer A, 1=buffer B
    output reg         busy,

    // PSRAM read port (req/ack, 16-bit words)
    output wire        rd_req,
    output wire [31:0] rd_addr,
    input  wire        rd_ack,
    input  wire [15:0] rd_data,

    // staging BRAM write (the bytes the SNES DMAs)
    output reg         st_we,
    output reg  [15:0] st_addr,
    output reg  [15:0] st_data,

    // DMA descriptor out to the scene engine (src,size,vdst,vmain)
    output reg         desc_we,
    output reg  [15:0] desc_src,
    output reg  [15:0] desc_size,
    output reg  [15:0] desc_vdst,
    output reg  [7:0]  desc_vmain
);
    localparam [3:0] F_CHRBASE=0, F_TMAPBASE=1, F_CHRPS=2, F_TMAPPS=3,
                     F_STAGE=4, F_VCHR=5, F_VOVL=6, F_VTMAP=7;
    localparam [7:0] VMAIN_STEP1 = 8'h80;

    // ---- config ----
    reg [31:0] chr_base, tmap_base;    // PSRAM word bases of this frame's CHR / tilemap
    reg [15:0] chr_per_sub, tmap_per_sub;
    reg [15:0] stage_base;             // BRAM staging word base
    reg [15:0] vram_chr, vram_ovl, vram_tmap;
    always @(posedge clk) if (cfg_we) case (cfg_field)
        F_CHRBASE : chr_base    <= cfg_data;
        F_TMAPBASE: tmap_base   <= cfg_data;
        F_CHRPS   : chr_per_sub <= cfg_data[15:0];
        F_TMAPPS  : tmap_per_sub<= cfg_data[15:0];
        F_STAGE   : stage_base  <= cfg_data[15:0];
        F_VCHR    : vram_chr    <= cfg_data[15:0];
        F_VOVL    : vram_ovl    <= cfg_data[15:0];
        F_VTMAP   : vram_tmap   <= cfg_data[15:0];
        default: ;
    endcase

    // ---- working state ----
    reg [2:0]  sub;
    reg        parity;
    reg [31:0] src;        // current PSRAM read address
    reg [15:0] sp;         // staging write cursor
    reg [15:0] rem;        // words left in the current copy

    // sub-frame offsets (sub * per_sub) — the one multiply each
    wire [31:0] chr_off  = sub * chr_per_sub;
    wire [31:0] tmap_off = sub * tmap_per_sub;
    // double-buffered CHR VRAM base: buffer B overlaps one granule earlier
    wire [15:0] vchr_buf = parity ? (vram_chr - vram_ovl) : vram_chr;

    localparam [3:0] S_IDLE=0, S_C_SET=1, S_C_REQ=2, S_C_WAIT=3, S_C_DESC=4,
                     S_T_SET=5, S_T_REQ=6, S_T_WAIT=7, S_T_DESC=8, S_DONE=9;
    reg [3:0] state;

    assign rd_req  = (state==S_C_WAIT) || (state==S_T_WAIT);
    assign rd_addr = src;

    always @(posedge clk) begin
        if (rst) begin state<=S_IDLE; busy<=1'b0; st_we<=1'b0; desc_we<=1'b0; end
        else begin
            st_we<=1'b0; desc_we<=1'b0;
            case (state)
            S_IDLE: if (go) begin
                busy<=1'b1; sub<=go_sub; parity<=go_parity; state<=S_C_SET;
            end

            // ---- CHR sub-frame copy ----
            S_C_SET: begin
                src <= chr_base + chr_off;
                sp  <= stage_base;
                rem <= chr_per_sub;
                state <= (chr_per_sub==16'd0) ? S_C_DESC : S_C_REQ;
            end
            S_C_REQ: state<=S_C_WAIT;
            S_C_WAIT: if (rd_ack) begin
                st_we<=1'b1; st_addr<=sp; st_data<=rd_data;
                sp<=sp+16'd1; src<=src+32'd1;
                if (rem-16'd1==16'd0) state<=S_C_DESC; else begin rem<=rem-16'd1; state<=S_C_REQ; end
            end
            S_C_DESC: begin
                desc_we<=1'b1;
                desc_src <= stage_base;
                desc_size<= {chr_per_sub[14:0],1'b0};       // words -> bytes
                desc_vdst<= vchr_buf + chr_off[15:0];
                desc_vmain<=VMAIN_STEP1;
                state<=S_T_SET;
            end

            // ---- tilemap sub-frame copy ----
            S_T_SET: begin
                src <= tmap_base + tmap_off;
                sp  <= stage_base + chr_per_sub;             // tilemap staged after CHR
                rem <= tmap_per_sub;
                state <= (tmap_per_sub==16'd0) ? S_DONE : S_T_REQ;
            end
            S_T_REQ: state<=S_T_WAIT;
            S_T_WAIT: if (rd_ack) begin
                st_we<=1'b1; st_addr<=sp; st_data<=rd_data;
                sp<=sp+16'd1; src<=src+32'd1;
                if (rem-16'd1==16'd0) state<=S_T_DESC; else begin rem<=rem-16'd1; state<=S_T_REQ; end
            end
            S_T_DESC: begin
                desc_we<=1'b1;
                desc_src <= stage_base + chr_per_sub;
                desc_size<= {tmap_per_sub[14:0],1'b0};
                desc_vdst<= vram_tmap + tmap_off[15:0];
                desc_vmain<=VMAIN_STEP1;
                state<=S_DONE;
            end
            S_DONE: begin busy<=1'b0; state<=S_IDLE; end
            default: state<=S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
