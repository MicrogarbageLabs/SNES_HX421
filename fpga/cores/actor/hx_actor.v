// ============================================================
//  hx_actor.v — actor priority engine: OAM depth-sort + overload flicker
//
//  Offloads the 65816's per-frame sprite work. Given an actor list (world
//  positions + sprite attributes), it assigns SNES OAM indices so that:
//    1. actors are transformed to screen space via the TOP layer's camera
//       (the single world frame — docs/tilemap-accelerator.md),
//    2. depth order is by screen Y-band (top-down: lower on screen = in front),
//       via an O(N) counting sort (histogram -> prefix -> scatter),
//    3. a PRIORITY-LOCK band (the first n_lock actors — player, boss, …) takes
//       the lowest OAM indices, so the PPU's per-line 32-sprite dropout (which
//       keeps the lowest indices) never flickers them,
//    4. the CROWD is admitted through a per-frame ROTATING WINDOW over the
//       depth-sorted list: each frame a different contiguous run of crowd ranks
//       gets the surviving low indices, so under heavy overload (>128 actors, or
//       >32 on a line) every crowd actor cycles through visibility instead of a
//       fixed set vanishing. The PPU does the per-line drop; we rotate WHO wins.
//
//  This is the overload-flicker fix (the first cut dropped the same tail every
//  frame and duplicated the PPU's per-line logic). RESOURCE-REPRESENTATIVE
//  skeleton; OAM high-table packing and on-hardware rotation-rate tuning are
//  follow-ons. CC0.
// ============================================================

`default_nettype none

module hx_actor #(
    parameter integer N   = 128,   // OAM sprite slots
    parameter integer SH  = 240    // screen height
) (
    input  wire        clk,
    input  wire        rst,

    input  wire        cfg_we,
    input  wire [2:0]  cfg_field,   // 0 cam_x,1 cam_y,2 count,3 phase,4 oam_base,5 n_lock
    input  wire [31:0] cfg_data,

    // actor list: [15:0] world_x,[31:16] world_y,[40:32] tile,[48:41] attr,[49] size
    input  wire        act_we,
    input  wire [6:0]  act_idx,
    input  wire [63:0] act_data,

    input  wire        go,
    output reg         busy,

    output reg         oam_we,
    output reg  [9:0]  oam_addr,
    output reg  [7:0]  oam_data
);
    localparam [2:0] F_CAMX=0,F_CAMY=1,F_COUNT=2,F_PHASE=3,F_OAMB=4,F_NLOCK=5;
    localparam integer NB = (SH + 7) / 8;   // Y bands (~30)

    // ---- config ----
    reg signed [15:0] cam_x, cam_y;
    reg [7:0] count, phase, n_lock;
    reg [9:0] oam_base;
    always @(posedge clk) if (cfg_we) case (cfg_field)
        F_CAMX:cam_x<=cfg_data[15:0]; F_CAMY:cam_y<=cfg_data[15:0];
        F_COUNT:count<=cfg_data[7:0]; F_PHASE:phase<=cfg_data[7:0];
        F_OAMB:oam_base<=cfg_data[9:0]; F_NLOCK:n_lock<=cfg_data[7:0];
        default:;
    endcase

    // ---- actor store (BRAM) ----
    reg [63:0] actors [0:N-1];
    always @(posedge clk) if (act_we) actors[act_idx] <= act_data;

    reg [7:0] hist [0:NB-1];    // per-band count, then running crowd-rank base
    integer ci;

    reg [7:0]  a, oidx, lockcnt, cw, rstart, budget;
    reg [63:0] ad;
    reg signed [15:0] sx, sy;
    reg        onscr;
    reg [7:0]  yb, acc;
    reg [8:0]  pidx;
    reg [1:0]  estep;
    reg [3:0]  emit_ret;

    // combinational screen transform of `ad`
    wire signed [15:0] w_x = ad[15:0];
    wire signed [15:0] w_y = ad[31:16];
    wire signed [15:0] c_sx = w_x - cam_x;
    wire signed [15:0] c_sy = w_y - cam_y;
    wire c_on = (c_sx > -16) && (c_sx < 256) && (c_sy >= 0) && (c_sy < SH);
    wire [7:0] c_yb = {3'd0, c_sy[7:3]};

    // crowd depth rank of the current band, and its rotated position this frame:
    // rot = (rank - rstart) mod cw  -> the per-frame rotating admission window.
    wire [7:0] rank_now = hist[yb];
    wire [7:0] rot_w = (rank_now >= rstart) ? (rank_now - rstart)
                                            : (rank_now - rstart + cw);

    localparam [3:0] S_IDLE=0,S_CLR=1,
                     S_LK_RD=2,S_LK_CALC=3,
                     S_H_RD=4,S_H_UP=5,S_PFX=6,S_RED=7,
                     S_SC_RD=8,S_SC_CALC=9,S_SC_ASSIGN=10,
                     S_EMIT=11,S_NEXT=12,S_DONE=13;
    reg [3:0] state;

    always @(posedge clk) begin
        if (rst) begin state<=S_IDLE; busy<=1'b0; oam_we<=1'b0; end
        else begin
            oam_we<=1'b0;
            case (state)
            S_IDLE: if (go) begin busy<=1'b1; ci<=0; lockcnt<=8'd0; cw<=8'd0; state<=S_CLR; end
            S_CLR: begin
                hist[ci]<=8'd0;
                if (ci==NB-1) begin a<=8'd0; ci<=0; state<=S_LK_RD; end else ci<=ci+1;
            end

            // ---- priority-lock pass: first n_lock on-screen actors -> low OAM indices ----
            S_LK_RD: if (a>=n_lock || a>=count) begin a<=8'd0; state<=S_H_RD; end
                     else begin ad<=actors[a]; state<=S_LK_CALC; end
            S_LK_CALC: begin
                if (c_on && lockcnt<N) begin
                    sx<=c_sx; sy<=c_sy; oidx<=lockcnt; lockcnt<=lockcnt+8'd1;
                    estep<=2'd0; emit_ret<=S_LK_RD; state<=S_EMIT;
                    a<=a+8'd1;
                end else begin a<=a+8'd1; state<=S_LK_RD; end
            end

            // ---- histogram the crowd (idx >= n_lock) by band; count cw ----
            S_H_RD: if (a>=count) begin pidx<=0; acc<=8'd0; state<=S_PFX; end
                    else begin ad<=actors[a]; state<=S_H_UP; end
            S_H_UP: begin
                if (a>=n_lock && c_on) begin hist[c_yb]<=hist[c_yb]+8'd1; cw<=cw+8'd1; end
                a<=a+8'd1; state<=S_H_RD;
            end
            // prefix: hist[band] -> starting crowd-rank for that band
            S_PFX: if (pidx==NB) begin
                       budget <= (N > lockcnt) ? (N - lockcnt) : 8'd0;
                       rstart <= phase; state<=S_RED;
                   end else begin
                       hist[pidx]<=acc; acc<=acc+hist[pidx]; pidx<=pidx+9'd1;
                   end
            // reduce rstart mod cw (frame rotation origin over the crowd)
            S_RED: if (cw!=8'd0 && rstart>=cw) rstart<=rstart-cw;
                   else begin a<=8'd0; state<=S_SC_RD; end

            // ---- scatter the crowd: depth rank -> rotating admission window ----
            S_SC_RD: if (a>=count) state<=S_DONE;
                     else begin ad<=actors[a]; state<=S_SC_CALC; end
            S_SC_CALC: begin sx<=c_sx; sy<=c_sy; onscr<=(a>=n_lock)&&c_on; yb<=c_yb; state<=S_SC_ASSIGN; end
            S_SC_ASSIGN: begin
                if (onscr) begin
                    hist[yb] <= hist[yb] + 8'd1;   // consume this band's depth rank
                    if (rot_w < budget) begin      // inside the rotating admission window
                        oidx  <= lockcnt + rot_w;
                        estep <= 2'd0; emit_ret<=S_NEXT; state<=S_EMIT;
                        a<=a+8'd1;
                    end else begin a<=a+8'd1; state<=S_SC_RD; end   // outside: flickered off this frame
                end else begin a<=a+8'd1; state<=S_SC_RD; end
            end

            // ---- shared OAM emit (4 low-table bytes) ----
            S_EMIT: begin
                oam_we<=1'b1;
                oam_addr <= oam_base + {oidx,2'b00} + {8'd0,estep};
                case (estep)
                    2'd0: oam_data <= sx[7:0];
                    2'd1: oam_data <= sy[7:0];
                    2'd2: oam_data <= ad[39:32];
                    default: oam_data <= ad[48:41];
                endcase
                if (estep==2'd3) state<=emit_ret; else estep<=estep+2'd1;
            end
            S_NEXT: state<=S_SC_RD;
            S_DONE: begin busy<=1'b0; state<=S_IDLE; end
            default: state<=S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
