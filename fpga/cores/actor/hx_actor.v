// ============================================================
//  hx_actor.v — actor priority engine: OAM depth-sort + flicker rotation
//
//  Offloads the 65816's per-frame sprite work. Given an actor list (world
//  positions + sprite attributes), it:
//    1. transforms each actor to screen space via the TOP layer's camera
//       (the single world frame — see docs/tilemap-accelerator.md),
//    2. depth-sorts by screen Y (top-down: lower on screen = drawn in front)
//       with an O(N) counting sort (histogram -> prefix -> scatter), NOT a
//       comparison network, so 128 actors cost ~linear time and little logic,
//    3. emits the SNES OAM low table in that priority order,
//    4. flicker-rotates the per-scanline overflow: the SNES drops sprites past
//       32 per line, so instead of the same ones vanishing, a per-frame phase
//       rotates which over-limit sprites are demoted — everything stays visible.
//
//  RESOURCE-REPRESENTATIVE skeleton for the LE/M9K tally: actor store, the
//  screen transform, the counting sort, OAM emission, and the flicker line-count
//  + phase rotation are the real cost. Exact OAM high-table packing and the
//  per-sprite vertical-span line accounting are follow-ons. CC0.
// ============================================================

`default_nettype none

module hx_actor #(
    parameter integer N   = 128,   // max actors / OAM sprites
    parameter integer SH  = 240,   // screen height (Y buckets)
    parameter integer LINE_MAX = 32 // SNES sprites-per-scanline limit
) (
    input  wire        clk,
    input  wire        rst,

    // config
    input  wire        cfg_we,
    input  wire [2:0]  cfg_field,   // 0 cam_x,1 cam_y,2 count,3 phase,4 oam_base
    input  wire [31:0] cfg_data,

    // actor list write, packed act_data =
    //   [15:0] world_x, [31:16] world_y, [40:32] tile[8:0], [48:41] attr[7:0],
    //   [49] size, [63:50] reserved
    input  wire        act_we,
    input  wire [6:0]  act_idx,
    input  wire [63:0] act_data,

    input  wire        go,
    output reg         busy,

    // OAM staging write port (external 544 B buffer)
    output reg         oam_we,
    output reg  [9:0]  oam_addr,
    output reg  [7:0]  oam_data
);
    localparam [2:0] F_CAMX=0, F_CAMY=1, F_COUNT=2, F_PHASE=3, F_OAMB=4;

    // ---- config ----
    reg signed [15:0] cam_x, cam_y;
    reg [7:0] count;
    reg [7:0] phase;
    reg [9:0] oam_base;
    always @(posedge clk) if (cfg_we) case (cfg_field)
        F_CAMX : cam_x    <= cfg_data[15:0];
        F_CAMY : cam_y    <= cfg_data[15:0];
        F_COUNT: count    <= cfg_data[7:0];
        F_PHASE: phase    <= cfg_data[7:0];
        F_OAMB : oam_base <= cfg_data[9:0];
        default: ;
    endcase

    // ---- actor store (BRAM) ----
    reg [63:0] actors [0:N-1];
    always @(posedge clk) if (act_we) actors[act_idx] <= act_data;

    // ---- sort / flicker state ----
    // Depth-sort by Y-BAND (Y>>3), not every scanline: 8-line granularity is
    // visually fine for sprite draw order and keeps the histogram/occupancy
    // arrays small (a full 256-entry version synthesized to ~6.4k LE of muxes
    // instead of BRAM — banding is the fix).
    localparam integer NB = (SH + 7) / 8;   // Y bands (~30 for 240 lines)
    reg [7:0]  hist [0:NB-1];    // per-band count, then repurposed as running index
    reg [7:0]  linek[0:NB-1];    // per-band occupancy (saturates at LINE_MAX)

    integer ci;

    // ---- pipeline registers for the actor currently being processed ----
    reg [7:0]  a;                // actor index
    reg [63:0] ad;               // fetched actor
    reg signed [15:0] sx, sy;    // screen coords
    reg        onscr;
    reg [7:0]  yb;               // Y band (0..NB-1)
    reg [7:0]  acc;              // prefix accumulator
    reg [8:0]  pidx;             // prefix walk index (0..NB)
    reg [7:0]  oidx;             // assigned OAM index
    reg [1:0]  estep;            // emit byte step

    localparam [3:0] S_IDLE=0,S_CLR=1,
                     S_H_RD=2,S_H_UP=3,
                     S_PFX=4,
                     S_S_RD=5,S_S_CALC=6,S_S_ASSIGN=7,S_EMIT=8,S_S_NEXT=9,
                     S_DONE=10;
    reg [3:0] state;

    // combinational screen transform of the fetched actor `ad`
    wire signed [15:0] w_x = ad[15:0];
    wire signed [15:0] w_y = ad[31:16];
    wire signed [15:0] c_sx = w_x - cam_x;
    wire signed [15:0] c_sy = w_y - cam_y;
    wire c_on = (c_sx > -16) && (c_sx < 256) && (c_sy >= 0) && (c_sy < SH);
    wire [7:0] c_yb = {3'd0, c_sy[7:3]};   // Y band = Y>>3

    // flicker demote: an over-limit sprite is dropped this frame if its
    // (index + phase) selects it — rotates the demoted set each frame.
    wire line_full = (linek[yb] >= LINE_MAX);
    wire demote    = line_full && (((oidx + phase) & 8'h03) == 8'h00);

    always @(posedge clk) begin
        if (rst) begin
            state<=S_IDLE; busy<=1'b0; oam_we<=1'b0;
        end else begin
            oam_we<=1'b0;
            case (state)
            S_IDLE: if (go) begin busy<=1'b1; ci<=0; state<=S_CLR; end
            S_CLR: begin
                // zero histogram + band occupancy (one band per cycle)
                hist[ci] <= 8'd0; linek[ci] <= 8'd0;
                if (ci==NB-1) begin a<=8'd0; ci<=0; state<=S_H_RD; end
                else ci<=ci+1;
            end
            // ---- histogram pass ----
            S_H_RD: begin ad<=actors[a]; state<=S_H_UP; end
            S_H_UP: begin
                if (c_on) hist[c_yb] <= hist[c_yb] + 8'd1;
                if (a+8'd1>=count) begin pidx<=0; acc<=phase; state<=S_PFX; end
                else begin a<=a+8'd1; state<=S_H_RD; end
            end
            // ---- prefix sum: hist[y] becomes the starting OAM index for Y ----
            S_PFX: begin
                if (pidx==NB) begin a<=8'd0; state<=S_S_RD; end
                else begin
                    // acc = running start (seeded with phase for coarse rotation)
                    hist[pidx] <= acc;
                    acc <= acc + hist[pidx];
                    pidx <= pidx + 9'd1;
                end
            end
            // ---- scatter pass: assign OAM index, emit or flicker-demote ----
            S_S_RD: begin ad<=actors[a]; state<=S_S_CALC; end
            S_S_CALC: begin
                sx<=c_sx; sy<=c_sy; onscr<=c_on; yb<=c_yb;
                state<=S_S_ASSIGN;
            end
            S_S_ASSIGN: begin
                if (onscr) begin
                    oidx <= hist[yb];
                    hist[yb] <= hist[yb] + 8'd1;
                    if (linek[yb] < LINE_MAX) linek[yb] <= linek[yb] + 8'd1;
                    estep<=2'd0;
                    state<=S_EMIT;
                end else state<=S_S_NEXT;
            end
            S_EMIT: begin
                // drop if over the OAM cap or flicker-demoted this frame
                if (oidx >= N || demote) begin
                    state<=S_S_NEXT;
                end else begin
                    oam_we<=1'b1;
                    oam_addr <= oam_base + {oidx,2'b00} + {8'd0,estep};
                    case (estep)
                        2'd0: oam_data <= sx[7:0];          // X low
                        2'd1: oam_data <= sy[7:0];          // Y
                        2'd2: oam_data <= ad[39:32];        // tile low 8
                        default: oam_data <= ad[48:41];     // attr byte
                    endcase
                    if (estep==2'd3) state<=S_S_NEXT;
                    else estep<=estep+2'd1;
                end
            end
            S_S_NEXT: begin
                if (a+8'd1>=count) state<=S_DONE;
                else begin a<=a+8'd1; state<=S_S_RD; end
            end
            S_DONE: begin busy<=1'b0; state<=S_IDLE; end
            default: state<=S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
