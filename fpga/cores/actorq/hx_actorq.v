// ============================================================
//  hx_actorq.v — bulk actor->map metatile query (RTL skeleton)
//
//  Realizes the actor<->layer query service (docs/tilemap-accelerator.md) as a
//  bulk per-actor op: for each actor, fetch the metatile index under it on the
//  queried layer and write it to a results BRAM the 65816 reads. Offloads the
//  per-actor PSRAM map lookup (collision, terrain, triggers) that the SNES cannot
//  do itself (it never reads PSRAM). The 65816 still DISPATCHES (which actors,
//  which layer, how to react); the FPGA just does the fetches.
//
//  Per actor a: query tile = (world - cam) >> 3  (cam = the layer's effective
//  offset; 0 for the top layer, top_cam-L_cam for a parallax layer). Then
//  mtx = qtx>>shift, mty = qty>>shift, result = PSRAM[map_base + mty*map_w + mtx]
//  (or oob sentinel off-map). One PSRAM read per actor — the metatile INDEX, not
//  the expanded tile, which is what map-aware game logic wants.
//
//  Point query here (1 tile/actor); a footprint is the same loop over the box's
//  corners. Address is pipelined (register decode -> multiply -> addr) for
//  timing, like hx_strip. RESOURCE-REPRESENTATIVE skeleton. CC0.
// ============================================================

`default_nettype none

module hx_actorq #(
    parameter integer NA = 128        // max actors
) (
    input  wire        clk,
    input  wire        rst,

    input  wire        cfg_we,
    input  wire [2:0]  cfg_field,     // 0 cam_x,1 cam_y,2 map_base,3 map_w,4 flags(shift,wrap,oob),5 res_base,6 count
    input  wire [31:0] cfg_data,

    // actor positions: [15:0] world_x, [31:16] world_y
    input  wire        act_we,
    input  wire [6:0]  act_idx,
    input  wire [31:0] act_data,

    input  wire        go,
    output reg         busy,

    // PSRAM read (shared via the arbiter with strip/FMV)
    output reg  [31:0] rd_addr,
    output wire        rd_req,
    input  wire        rd_ack,
    input  wire [15:0] rd_data,

    // results BRAM write (metatile index per actor)
    output reg         res_we,
    output reg  [15:0] res_addr,
    output reg  [15:0] res_data
);
    localparam [2:0] F_CAMX=0,F_CAMY=1,F_MAP=2,F_W=3,F_FLAGS=4,F_RES=5,F_COUNT=6;

    reg signed [15:0] cam_x, cam_y;
    reg [31:0] map_base;
    reg [15:0] map_w, map_h, oob_val, res_base;
    reg [2:0]  shift;
    reg        wrap;
    reg [7:0]  count;
    always @(posedge clk) if (cfg_we) case (cfg_field)
        F_CAMX : cam_x    <= cfg_data[15:0];
        F_CAMY : cam_y    <= cfg_data[15:0];
        F_MAP  : map_base <= cfg_data;
        F_W    : begin map_w <= cfg_data[15:0]; map_h <= cfg_data[31:16]; end
        F_FLAGS: begin shift<=cfg_data[2:0]; wrap<=cfg_data[3]; oob_val<=cfg_data[31:16]; end
        F_RES  : res_base <= cfg_data[15:0];
        F_COUNT: count    <= cfg_data[7:0];
        default: ;
    endcase

    // actor position store
    reg [31:0] apos [0:NA-1];
    always @(posedge clk) if (act_we) apos[act_idx] <= act_data;

    localparam [3:0] S_IDLE=0,S_RD=1,S_DEC=2,S_ADDR=3,S_REQ=4,S_WAIT=5,S_WR=6,S_DONE=7;
    reg [3:0] state;

    reg [7:0]  a;
    reg [31:0] ad;
    reg [15:0] r_mtx, r_mty;
    reg        r_oob;

    wire signed [15:0] w_x = ad[15:0];
    wire signed [15:0] w_y = ad[31:16];
    wire signed [15:0] qtx = (w_x - cam_x) >>> 3;   // world tile X
    wire signed [15:0] qty = (w_y - cam_y) >>> 3;
    wire signed [15:0] c_mtx = qtx >>> shift;
    wire signed [15:0] c_mty = qty >>> shift;
    wire c_oob = (!wrap) && ((c_mtx<0)||(c_mty<0)
                          || (c_mtx>=$signed({1'b0,map_w}))||(c_mty>=$signed({1'b0,map_h})));
    wire [31:0] map_index = (r_mty * map_w) + {16'd0,r_mtx};

    assign rd_req = (state==S_WAIT);

    always @(posedge clk) begin
        if (rst) begin state<=S_IDLE; busy<=1'b0; res_we<=1'b0; end
        else begin
            res_we<=1'b0;
            case (state)
            S_IDLE: if (go) begin busy<=1'b1; a<=8'd0; state<=S_RD; end
            S_RD:   if (a>=count) state<=S_DONE; else begin ad<=apos[a]; state<=S_DEC; end
            S_DEC:  begin r_mtx<=c_mtx[15:0]; r_mty<=c_mty[15:0]; r_oob<=c_oob; state<=S_ADDR; end
            S_ADDR: begin
                if (r_oob) begin res_data<=oob_val; state<=S_WR; end
                else begin rd_addr <= map_base + map_index; state<=S_REQ; end
            end
            S_REQ:  state<=S_WAIT;
            S_WAIT: if (rd_ack) begin res_data<=rd_data; state<=S_WR; end
            S_WR:   begin
                res_we<=1'b1; res_addr<=res_base + {8'd0,a};
                a<=a+8'd1; state<=S_RD;
            end
            S_DONE: begin busy<=1'b0; state<=S_IDLE; end
            default: state<=S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
