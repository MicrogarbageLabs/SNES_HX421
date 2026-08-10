// ============================================================
//  hx_strip.v — multi-layer map strip builder / metatile fetch (RTL skeleton)
//
//  Hardware form of runtime/hx421_metatile.c + hx_map_layer_goto(): a 3-layer
//  Mode-1 tilemap accelerator. One fetch/expand datapath is time-multiplexed
//  over per-layer context (mixer load-context style), so each BG has its own
//  map pointer, camera and sliding window at modest LE cost. See
//  docs/tilemap-accelerator.md.
//
//  Per output tile (tx,ty), matching mt_lookup:
//    mtx=tx>>shift; mty=ty>>shift
//    mt  = PSRAM[map_base + mty*map_w + mtx]                 (metatile index)
//    entry = (mt>=def_count) ? oob
//          : PSRAM[defs_base + (mt<<2*shift)+(sy<<shift)+sx] (tilemap word)
//
//  Per layer_goto(li,nx,ny), matching hx_map_layer_goto:
//    clamp nx,ny to the layer limits if clamp mode (static screen / HUD sub-map)
//    want_l=(nx>>3)-16; want_t=(ny>>3)-2; dl=want_l-win_l; dt=want_t-win_t
//    reseed if !seeded || |dl|>MAX_COLS || |dt|>MAX_ROWS, else emit |dl| columns
//    (entering in the travel direction) + |dt| rows; update the window
//
//  RESOURCE-REPRESENTATIVE skeleton for the LE tally (control + per-layer state
//  + the measured fetch datapath). Not yet co-simulated against the C reference;
//  the transposed-column path and DMA-descriptor emission are follow-ons. CC0.
// ============================================================

`default_nettype none

module hx_strip #(
    parameter integer NL = 3,          // layers (Mode 1: BG1,BG2,BG3)
    parameter integer MAX_COLS = 8,
    parameter integer MAX_ROWS = 4
) (
    input  wire        clk,
    input  wire        rst,

    input  wire        cfg_we,
    input  wire [1:0]  cfg_layer,
    input  wire [4:0]  cfg_field,
    input  wire [31:0] cfg_data,

    input  wire        go,
    input  wire [1:0]  go_layer,
    input  wire signed [15:0] go_x,
    input  wire signed [15:0] go_y,
    output reg         busy,

    output wire        rd_req,
    output reg  [31:0] rd_addr,
    input  wire        rd_ack,
    input  wire [15:0] rd_data,

    output reg         strip_we,
    output reg  [15:0] strip_addr,
    output reg  [15:0] strip_data
);
    localparam [4:0] F_MAP=0,F_COLS=1,F_DEFS=2,F_W=3,F_H=4,F_DEFCNT=5,
                     F_FLAGS=6,F_STRIP=7,F_XMIN=8,F_XMAX=9,F_YMIN=10,F_YMAX=11;
    localparam [3:0] S_IDLE=0,S_LOAD=1,S_CALC=2,S_SEGSET=3,S_SEGCHK=4,S_TSET=5,
                     S_MREQ=6,S_MWAIT=7,S_DWAIT=8,S_TW=9,S_TNEXT=10,
                     S_SEGNEXT=11,S_STORE=12,S_DEC=13,S_DADDR=14;
    reg [3:0] state;

    // ---- per-layer context ----
    reg [31:0] Lmap[0:NL-1], Lcols[0:NL-1], Ldefs[0:NL-1];
    reg [15:0] Lw[0:NL-1], Lh[0:NL-1], Ldc[0:NL-1], Loob[0:NL-1], Lstr[0:NL-1];
    reg [2:0]  Lsh[0:NL-1];
    reg        Lwrap[0:NL-1], Lclmp[0:NL-1], Lseed[0:NL-1];
    reg signed [15:0] Lwl[0:NL-1], Lwt[0:NL-1], Lcx[0:NL-1], Lcy[0:NL-1];
    reg signed [15:0] Lxmn[0:NL-1], Lxmx[0:NL-1], Lymn[0:NL-1], Lymx[0:NL-1];

    integer j;
    initial for (j=0;j<NL;j=j+1) begin Lseed[j]=1'b0; Lwl[j]=0; Lwt[j]=0; end

    always @(posedge clk) if (cfg_we) case (cfg_field)
        F_MAP:Lmap[cfg_layer]<=cfg_data; F_COLS:Lcols[cfg_layer]<=cfg_data;
        F_DEFS:Ldefs[cfg_layer]<=cfg_data; F_W:Lw[cfg_layer]<=cfg_data[15:0];
        F_H:Lh[cfg_layer]<=cfg_data[15:0]; F_DEFCNT:Ldc[cfg_layer]<=cfg_data[15:0];
        F_FLAGS:begin Lsh[cfg_layer]<=cfg_data[2:0]; Lwrap[cfg_layer]<=cfg_data[3];
                      Lclmp[cfg_layer]<=cfg_data[4]; Loob[cfg_layer]<=cfg_data[31:16]; end
        F_STRIP:Lstr[cfg_layer]<=cfg_data[15:0];
        F_XMIN:Lxmn[cfg_layer]<=cfg_data[15:0]; F_XMAX:Lxmx[cfg_layer]<=cfg_data[15:0];
        F_YMIN:Lymn[cfg_layer]<=cfg_data[15:0]; F_YMAX:Lymx[cfg_layer]<=cfg_data[15:0];
        default:;
    endcase

    // ---- working (loaded-context) registers ----
    reg [1:0]  li;
    reg [31:0] w_map, w_defs;
    reg [15:0] w_w, w_h, w_dc, w_oob, w_str;
    reg [2:0]  w_sh;
    reg        w_wrap;
    reg signed [15:0] w_wl, w_wt, want_l, want_t, dl, dt, nx, ny;

    // strip sequencing
    reg [15:0] seg, seg_n, out_ptr, tcnt, tlen;
    reg        seg_is_row, do_reseed;

    // inner fetch
    reg signed [15:0] tx, ty;
    reg [15:0] mt, entry;
    reg        cache_v, fdef;
    reg [15:0] cache_mtx, cache_mty, cache_mt;
    // registered tile decode (a pipeline stage: no shift+mult+add crosses one cycle)
    reg [15:0] r_mtx, r_mty;
    reg [2:0]  r_sx, r_sy;
    reg        r_oob;

    // combinational decode from (tx,ty) — latched into r_* in S_DEC
    wire [15:0] submask = (16'd1 << w_sh) - 16'd1;
    wire signed [15:0] c_mtx = tx >>> w_sh;
    wire signed [15:0] c_mty = ty >>> w_sh;
    wire [2:0] c_sx = tx[2:0] & submask[2:0];
    wire [2:0] c_sy = ty[2:0] & submask[2:0];
    wire c_oob = (!w_wrap) && ( (c_mtx < 0) || (c_mty < 0)
                            || (c_mtx >= $signed({1'b0,w_w}))
                            || (c_mty >= $signed({1'b0,w_h})) );
    wire [15:0] u_mtx = c_mtx[15:0];
    wire [15:0] u_mty = c_mty[15:0];
    // address ops now run reg(r_*) -> rd_addr(reg), so each is one op per cycle
    wire [31:0] map_index = (r_mty * w_w) + {16'd0,r_mtx};
    wire [3:0]  sh2 = {w_sh,1'b0};
    wire [31:0] def_index = ({16'd0,mt} << sh2) + ({29'd0,r_sy} << w_sh) + {29'd0,r_sx};

    assign rd_req  = (state==S_MWAIT) || (state==S_DWAIT);

    always @(posedge clk) begin
        if (rst) begin
            state<=S_IDLE; busy<=1'b0; strip_we<=1'b0;
        end else begin
            strip_we<=1'b0;
            case (state)
            S_IDLE: if (go) begin busy<=1'b1; li<=go_layer; nx<=go_x; ny<=go_y; state<=S_LOAD; end
            S_LOAD: begin
                w_map<=Lmap[li]; w_defs<=Ldefs[li]; w_w<=Lw[li]; w_h<=Lh[li];
                w_dc<=Ldc[li]; w_oob<=Loob[li]; w_str<=Lstr[li]; w_sh<=Lsh[li];
                w_wrap<=Lwrap[li]; w_wl<=Lwl[li]; w_wt<=Lwt[li];
                if (Lclmp[li]) begin
                    if (nx < Lxmn[li]) nx<=Lxmn[li]; else if (nx > Lxmx[li]) nx<=Lxmx[li];
                    if (ny < Lymn[li]) ny<=Lymn[li]; else if (ny > Lymx[li]) ny<=Lymx[li];
                end
                cache_v<=1'b0; state<=S_CALC;
            end
            S_CALC: begin
                want_l<=(nx>>>3)-16'sd16; want_t<=(ny>>>3)-16'sd2;
                dl<=((nx>>>3)-16'sd16)-w_wl; dt<=((ny>>>3)-16'sd2)-w_wt;
                state<=S_SEGSET;
            end
            S_SEGSET: begin
                do_reseed <= (!Lseed[li]) || (dl>MAX_COLS) || (-dl>MAX_COLS)
                                          || (dt>MAX_ROWS) || (-dt>MAX_ROWS);
                seg<=16'd0; tcnt<=16'd0; seg_is_row<=1'b0; out_ptr<=w_str;
                if ((!Lseed[li])||(dl>MAX_COLS)||(-dl>MAX_COLS)||(dt>MAX_ROWS)||(-dt>MAX_ROWS))
                     seg_n<=16'd64;                    // reseed: full window width
                else seg_n<=(dl[15]?-dl:dl);          // seams: |dl| columns
                state<=S_SEGCHK;
            end
            S_SEGCHK: begin
                if (seg_n==16'd0) begin
                    // no columns this pass -> try the row pass, else done
                    if (!seg_is_row && !do_reseed && (dt!=0)) begin
                        seg<=16'd0; tcnt<=16'd0; seg_is_row<=1'b1;
                        seg_n<=(dt[15]?-dt:dt);        // stays this state to re-check
                    end else state<=S_STORE;
                end else state<=S_TSET;
            end
            S_TSET: begin
                if (!seg_is_row) begin
                    tx <= do_reseed ? (want_l + seg)
                                    : (dl>0 ? (w_wl+16'sd64+seg) : (w_wl-16'sd1-seg));
                    ty <= want_t + tcnt;
                    tlen<=16'd32;
                end else begin
                    ty <= (dt>0 ? (w_wt+16'sd32+seg) : (w_wt-16'sd1-seg));
                    tx <= want_l + tcnt;
                    tlen<=16'd64;
                end
                state<=S_DEC;
            end
            // pipeline stage: latch the tile decode (shift) into registers
            S_DEC: begin
                r_mtx<=u_mtx; r_mty<=u_mty; r_sx<=c_sx; r_sy<=c_sy; r_oob<=c_oob;
                state<=S_MREQ;
            end
            S_MREQ: begin
                if (r_oob) begin entry<=w_oob; state<=S_TW; end
                else if (cache_v && cache_mtx==r_mtx && cache_mty==r_mty) begin
                    mt<=cache_mt; state<=S_DADDR;      // cache hit: skip the map read
                end else begin
                    rd_addr <= w_map + map_index;      // register the map read address
                    state<=S_MWAIT;
                end
            end
            S_MWAIT: if (rd_ack) begin
                mt<=rd_data; cache_v<=1'b1; cache_mtx<=r_mtx; cache_mty<=r_mty; cache_mt<=rd_data;
                if (rd_data >= w_dc) begin entry<=w_oob; state<=S_TW; end
                else state<=S_DADDR;
            end
            // register the def read address (mt/r_sx/r_sy -> rd_addr, one cycle)
            S_DADDR: begin rd_addr <= w_defs + def_index; state<=S_DWAIT; end
            S_DWAIT: if (rd_ack) begin entry<=rd_data; state<=S_TW; end
            S_TW: begin
                strip_we<=1'b1; strip_addr<=out_ptr; strip_data<=entry;
                out_ptr<=out_ptr+16'd1; state<=S_TNEXT;
            end
            S_TNEXT: if (tcnt+16'd1>=tlen) begin tcnt<=16'd0; state<=S_SEGNEXT; end
                     else begin tcnt<=tcnt+16'd1; state<=S_TSET; end
            S_SEGNEXT: begin
                if (seg+16'd1>=seg_n) begin
                    if (!seg_is_row && !do_reseed && (dt!=0)) begin
                        seg<=16'd0; tcnt<=16'd0; seg_is_row<=1'b1;
                        seg_n<=(dt[15]?-dt:dt); state<=S_SEGCHK;
                    end else state<=S_STORE;
                end else begin seg<=seg+16'd1; state<=S_TSET; end
            end
            S_STORE: begin
                Lwl[li]<=want_l; Lwt[li]<=want_t; Lcx[li]<=nx; Lcy[li]<=ny;
                Lseed[li]<=1'b1; busy<=1'b0; state<=S_IDLE;
            end
            default: state<=S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
