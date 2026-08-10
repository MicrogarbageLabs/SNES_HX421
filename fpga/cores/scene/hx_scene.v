// ============================================================
//  hx_scene.v — scene engine: composes the per-frame SNES DMA body (RTL skeleton)
//
//  The FPGA builds the 65816 program the SNES executes each NMI (execute-from-
//  window): PPU register writes (BGMODE/TM/TS/BGnSC/scroll/CGWSEL/…) followed by
//  a DMA slot per transfer (tilemap seams from the strip engine, OAM from the
//  actor engine, palette/CHR). This is the hardware form of hx421_runtime.c's
//  e_lda_sta* / e_dma_vram_slot emitters — the "NMI builder, in fabric".
//
//  Emitted opcodes (A 8/16-bit):
//    lda #imm8 ; sta abs16   -> A9 imm 8D lo hi                 (5 bytes)
//    lda #imm16; sta abs16   -> A9 lo hi 8D lo hi               (6 bytes)
//    rep/sep #$20            -> C2 20 / E2 20                    (2 bytes)
//  A VRAM DMA slot is the fixed sequence of those with src/size/vdst/vmain
//  patched in (see the micro-op table).
//
//  Inputs: a PPU register bank (addr,val,width) and a DMA-descriptor list
//  (src,size,vdst,vmain) the sub-engines populate. Output: the code byte stream
//  to the staging BRAM. RESOURCE-REPRESENTATIVE skeleton for the LE tally; the
//  descriptor list is filled by the strip/actor/FMV engines in the full system.
//  CC0.
// ============================================================

`default_nettype none

module hx_scene #(
    parameter integer NR = 16,     // PPU register-bank slots
    parameter integer ND = 32      // DMA descriptor slots
) (
    input  wire        clk,
    input  wire        rst,

    // PPU register bank write
    input  wire        reg_we,
    input  wire [3:0]  reg_idx,
    input  wire        reg_is16,
    input  wire [15:0] reg_addr,
    input  wire [15:0] reg_val,
    input  wire [4:0]  reg_count,

    // DMA descriptor list write
    input  wire        desc_we,
    input  wire [4:0]  desc_idx,
    input  wire [15:0] desc_src,
    input  wire [15:0] desc_size,
    input  wire [15:0] desc_vdst,
    input  wire [7:0]  desc_vmain,
    input  wire [5:0]  desc_count,

    input  wire        go,
    output reg         busy,

    // code output to the staging window (the 65816 DMA body)
    output reg         code_we,
    output reg  [15:0] code_addr,
    output reg  [7:0]  code_data
);
    // ---- SNES register addresses ----
    localparam [15:0] DMAP0=16'h4300, BBAD0=16'h4301, A1T0L=16'h4302,
                      DAS0L=16'h4305, VMAIN=16'h2115, VMADDL=16'h2116,
                      MDMAEN=16'h420B;

    // ---- register bank + descriptor list ----
    reg        rb_is16 [0:NR-1];
    reg [15:0] rb_addr [0:NR-1];
    reg [15:0] rb_val  [0:NR-1];
    always @(posedge clk) if (reg_we) begin
        rb_is16[reg_idx]<=reg_is16; rb_addr[reg_idx]<=reg_addr; rb_val[reg_idx]<=reg_val;
    end

    reg [15:0] d_src [0:ND-1];
    reg [15:0] d_size[0:ND-1];
    reg [15:0] d_vdst[0:ND-1];
    reg [7:0]  d_vmain[0:ND-1];
    always @(posedge clk) if (desc_we) begin
        d_src[desc_idx]<=desc_src; d_size[desc_idx]<=desc_size;
        d_vdst[desc_idx]<=desc_vdst; d_vmain[desc_idx]<=desc_vmain;
    end

    // ---- emit-engine indices (declared early: used by the combinational tables) ----
    reg [4:0]  ri;       // register-bank index
    reg [5:0]  di;       // descriptor index
    reg [3:0]  mo;       // micro-op index

    // ---- micro-op table for one DMA slot ----
    localparam [1:0] MO_R8=0, MO_R16=1, MO_REP=2, MO_SEP=3;   // op kinds
    localparam [2:0] IS_CONST=0, IS_SRC=1, IS_SIZE=2, IS_VDST=3, IS_VMAIN=4;
    localparam integer N_MO = 11;

    reg [1:0]  mo_op;
    reg [15:0] mo_imm;    // used when IS_CONST
    reg [2:0]  mo_sel;
    reg [15:0] mo_addr;
    always @(*) begin
        case (mo)
          4'd0 : begin mo_op=MO_R8;  mo_imm=16'h0018; mo_sel=IS_CONST; mo_addr=BBAD0;  end
          4'd1 : begin mo_op=MO_R8;  mo_imm=16'h0001; mo_sel=IS_CONST; mo_addr=DMAP0;  end
          4'd2 : begin mo_op=MO_REP; mo_imm=16'h0;    mo_sel=IS_CONST; mo_addr=16'h0;  end
          4'd3 : begin mo_op=MO_R16; mo_imm=16'h0;    mo_sel=IS_SRC;   mo_addr=A1T0L;  end
          4'd4 : begin mo_op=MO_R16; mo_imm=16'h0;    mo_sel=IS_SIZE;  mo_addr=DAS0L;  end
          4'd5 : begin mo_op=MO_SEP; mo_imm=16'h0;    mo_sel=IS_CONST; mo_addr=16'h0;  end
          4'd6 : begin mo_op=MO_R8;  mo_imm=16'h0;    mo_sel=IS_VMAIN; mo_addr=VMAIN;  end
          4'd7 : begin mo_op=MO_REP; mo_imm=16'h0;    mo_sel=IS_CONST; mo_addr=16'h0;  end
          4'd8 : begin mo_op=MO_R16; mo_imm=16'h0;    mo_sel=IS_VDST;  mo_addr=VMADDL; end
          4'd9 : begin mo_op=MO_SEP; mo_imm=16'h0;    mo_sel=IS_CONST; mo_addr=16'h0;  end
          default:begin mo_op=MO_R8; mo_imm=16'h0001; mo_sel=IS_CONST; mo_addr=MDMAEN; end
        endcase
    end

    // operand pick for the current micro-op (descriptor di)
    reg [15:0] op_imm;
    reg        op_is16;
    always @(*) begin
        op_is16 = (mo_op==MO_R16);
        case (mo_sel)
          IS_SRC  : op_imm = d_src [di];
          IS_SIZE : op_imm = d_size[di];
          IS_VDST : op_imm = d_vdst[di];
          IS_VMAIN: op_imm = {8'd0, d_vmain[di]};
          default : op_imm = mo_imm;
        endcase
    end

    // ---- emit engine ----
    reg [15:0] cptr;     // code write pointer
    reg [2:0]  bstep;    // byte step within a lda/sta emit
    reg [15:0] cur_imm, cur_addr;
    reg        cur_is16;
    reg [3:0]  ret;      // state to return to after an lda/sta emit
    reg        raw_hi;   // second byte of a rep/sep

    localparam [3:0] S_IDLE=0,S_PRE=1,S_PRE_NEXT=2,S_SLOT=3,S_MO=4,S_MO_NEXT=5,
                     S_LD=6,S_RAW=7,S_DONE=8;
    reg [3:0] state;

    task put; input [7:0] b; begin
        code_we<=1'b1; code_addr<=cptr; code_data<=b; cptr<=cptr+16'd1;
    end endtask

    always @(posedge clk) begin
        if (rst) begin state<=S_IDLE; busy<=1'b0; code_we<=1'b0; end
        else begin
            code_we<=1'b0;
            case (state)
            S_IDLE: if (go) begin busy<=1'b1; cptr<=16'd0; ri<=5'd0; state<=S_PRE; end

            // ---- PPU register preamble: lda #val ; sta addr, per bank entry ----
            S_PRE: if (ri >= reg_count) begin di<=6'd0; state<=S_SLOT; end
                   else begin
                       cur_imm<=rb_val[ri]; cur_addr<=rb_addr[ri]; cur_is16<=rb_is16[ri];
                       bstep<=3'd0; ret<=S_PRE_NEXT; state<=S_LD;
                   end
            S_PRE_NEXT: begin ri<=ri+5'd1; state<=S_PRE; end

            // ---- one DMA slot per descriptor ----
            S_SLOT: if (di >= desc_count) state<=S_DONE;
                    else begin mo<=4'd0; state<=S_MO; end
            S_MO: begin
                if (mo_op==MO_REP || mo_op==MO_SEP) begin
                    put(mo_op==MO_REP ? 8'hC2 : 8'hE2);
                    raw_hi<=1'b1; state<=S_RAW;
                end else begin
                    cur_imm<=op_imm; cur_addr<=mo_addr; cur_is16<=op_is16;
                    bstep<=3'd0; ret<=S_MO_NEXT; state<=S_LD;
                end
            end
            S_RAW: begin put(8'h20); state<=S_MO_NEXT; end
            S_MO_NEXT: if (mo == N_MO-1) begin di<=di+6'd1; state<=S_SLOT; end
                       else begin mo<=mo+4'd1; state<=S_MO; end

            // ---- lda #imm ; sta abs16 byte emitter ----
            S_LD: begin
                case (bstep)
                    3'd0: begin put(8'hA9); bstep<=3'd1; end          // LDA #
                    3'd1: begin put(cur_imm[7:0]);
                                if (cur_is16) bstep<=3'd2; else bstep<=3'd3; end
                    3'd2: begin put(cur_imm[15:8]); bstep<=3'd3; end  // imm high (A16)
                    3'd3: begin put(8'h8D); bstep<=3'd4; end          // STA abs
                    3'd4: begin put(cur_addr[7:0]); bstep<=3'd5; end
                    default: begin put(cur_addr[15:8]); state<=ret; end
                endcase
            end
            S_DONE: begin busy<=1'b0; state<=S_IDLE; end
            default: state<=S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
