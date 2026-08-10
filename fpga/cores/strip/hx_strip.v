// ============================================================
//  hx_strip.v — map strip builder / metatile fetch (RTL skeleton)
//
//  Hardware form of runtime/hx421_metatile.c mt_lookup(): expand a strip of
//  SNES BG tilemap entries from a metatile map in PSRAM, so per-frame DMA is an
//  edge column/row (64-128 B) instead of the whole 2 KB tilemap. See
//  docs/tilemap-accelerator.md.
//
//  Per output tile (tx,ty):
//    mtx = tx>>shift; mty = ty>>shift            (shift = log2(mt_side), 1/2/3)
//    map_index = mty*map_w + mtx                  (the one true multiply)
//    mt  = PSRAM[map_base + map_index]            (metatile index, 16b)
//    if (mt >= def_count) entry = oob_entry
//    else entry = PSRAM[defs_base + (mt<<2*shift) + (sy<<shift) + sx]
//    strip[strip_base + i] = entry                (16b SNES tilemap word)
//
//  A 1-deep metatile cache (last mtx,mty→mt) skips the map fetch while walking
//  within a metatile (a column crosses a metatile boundary only every `side`
//  tiles), so most tiles cost one PSRAM read, not two — the realistic datapath.
//
//  This is a RESOURCE-REPRESENTATIVE skeleton for the LE/M9K tally: the control
//  FSM, two-level fetch + cache, and address arithmetic are the real cost. Full
//  co-sim against the C reference is a later step. Public domain (CC0).
// ============================================================

`default_nettype none

module hx_strip (
    input  wire        clk,
    input  wire        rst,

    // config register file (write one field at a time)
    input  wire        cfg_we,
    input  wire [3:0]  cfg_field,   // see localparams below
    input  wire [31:0] cfg_data,

    // start / status
    input  wire        go,          // 1-cycle pulse: build the strip
    output reg         busy,

    // PSRAM read port (single req/ack, 16-bit words) — same shape as the mixer
    output wire        rd_req,
    output wire [31:0] rd_addr,
    input  wire        rd_ack,
    input  wire [15:0] rd_data,

    // strip staging write port (to external BRAM the SNES DMAs from)
    output reg         strip_we,
    output reg  [15:0] strip_addr,
    output reg  [15:0] strip_data
);

    // ---- config fields ----
    localparam [3:0] F_MAP_BASE  = 4'd0;
    localparam [3:0] F_DEFS_BASE = 4'd1;
    localparam [3:0] F_MAP_W     = 4'd2;
    localparam [3:0] F_MAP_H     = 4'd3;
    localparam [3:0] F_DEF_COUNT = 4'd4;
    localparam [3:0] F_FLAGS     = 4'd5;  // [2:0]=shift, [3]=dir(0 col/1 row), [4]=wrap, [31:16]=oob
    localparam [3:0] F_FIXED     = 4'd6;  // column: tx ; row: ty
    localparam [3:0] F_START     = 4'd7;  // column: ty0; row: tx0
    localparam [3:0] F_COUNT     = 4'd8;
    localparam [3:0] F_STRIP     = 4'd9;  // strip staging base (word addr)

    reg [31:0] map_base, defs_base;
    reg [15:0] map_w, map_h, def_count;
    reg [2:0]  shift;
    reg        dir, wrap;
    reg [15:0] oob_entry;
    reg [15:0] fixed_c, start_c, count;
    reg [15:0] strip_base;

    always @(posedge clk) begin
        if (cfg_we) case (cfg_field)
            F_MAP_BASE : map_base   <= cfg_data;
            F_DEFS_BASE: defs_base  <= cfg_data;
            F_MAP_W    : map_w      <= cfg_data[15:0];
            F_MAP_H    : map_h      <= cfg_data[15:0];
            F_DEF_COUNT: def_count  <= cfg_data[15:0];
            F_FLAGS    : begin shift <= cfg_data[2:0]; dir <= cfg_data[3];
                               wrap <= cfg_data[4];    oob_entry <= cfg_data[31:16]; end
            F_FIXED    : fixed_c    <= cfg_data[15:0];
            F_START    : start_c    <= cfg_data[15:0];
            F_COUNT    : count      <= cfg_data[15:0];
            F_STRIP    : strip_base <= cfg_data[15:0];
            default: ;
        endcase
    end

    // ---- walk state ----
    reg [15:0] i;              // output index 0..count-1
    reg [15:0] tx, ty;        // current tile coords
    reg [15:0] mtx, mty;      // metatile coords
    reg [2:0]  sx, sy;        // sub-tile within metatile
    reg [15:0] mt;            // current metatile index (from map fetch / cache)

    // 1-deep metatile cache
    reg        cache_valid;
    reg [15:0] cache_mtx, cache_mty, cache_mt;

    wire [15:0] submask = (16'd1 << shift) - 16'd1;

    // oob test (non-wrap): any coord outside the map
    wire oob = (!wrap) && ( (mtx >= map_w) || (mty >= map_h) );

    // map index = mty*map_w + mtx  (the one real multiply -> DSP)
    wire [31:0] map_index = (mty * map_w) + {16'd0, mtx};
    // def index = (mt << 2*shift) + (sy << shift) + sx
    wire [3:0]  shift2    = {shift, 1'b0};              // 2*shift
    wire [31:0] def_index = ({16'd0, mt} << shift2)
                          + ({29'd0, sy} << shift)
                          + {29'd0, sx};

    localparam [3:0] S_IDLE=0, S_SETUP=1, S_META_REQ=2, S_META_WAIT=3,
                     S_DEF_REQ=4, S_DEF_WAIT=5, S_WRITE=6, S_NEXT=7, S_DONE=8;
    reg [3:0] state;
    reg       fetching_def;    // 1 = current read is the def fetch, 0 = map fetch
    reg [15:0] entry;
    reg        use_oob;

    assign rd_req  = (state == S_META_REQ) || (state == S_DEF_REQ);
    assign rd_addr = fetching_def ? (defs_base + def_index)
                                  : (map_base  + map_index);

    always @(posedge clk) begin
        if (rst) begin
            state <= S_IDLE; busy <= 1'b0; strip_we <= 1'b0;
            cache_valid <= 1'b0; i <= 16'd0;
        end else begin
            strip_we <= 1'b0;
            case (state)
                S_IDLE: if (go) begin
                    busy <= 1'b1;
                    i    <= 16'd0;
                    // seed coords from the walk direction
                    tx   <= dir ? start_c : fixed_c;
                    ty   <= dir ? fixed_c : start_c;
                    cache_valid <= 1'b0;
                    state <= S_SETUP;
                end
                S_SETUP: begin
                    mtx <= tx >> shift;
                    mty <= ty >> shift;
                    sx  <= tx[2:0] & submask[2:0];
                    sy  <= ty[2:0] & submask[2:0];
                    use_oob <= 1'b0;
                    state <= S_META_REQ;
                end
                S_META_REQ: begin
                    if (oob) begin
                        entry   <= oob_entry;
                        use_oob <= 1'b1;
                        state   <= S_WRITE;
                    end else if (cache_valid && cache_mtx == mtx && cache_mty == mty) begin
                        mt    <= cache_mt;         // cache hit: skip the map read
                        fetching_def <= 1'b1;
                        state <= S_DEF_REQ;
                    end else begin
                        fetching_def <= 1'b0;      // map read
                        state <= S_META_WAIT;
                    end
                end
                S_META_WAIT: if (rd_ack) begin
                    mt          <= rd_data;
                    cache_valid <= 1'b1;
                    cache_mtx   <= mtx;
                    cache_mty   <= mty;
                    cache_mt    <= rd_data;
                    if (rd_data >= def_count) begin
                        entry <= oob_entry; use_oob <= 1'b1; state <= S_WRITE;
                    end else begin
                        fetching_def <= 1'b1; state <= S_DEF_REQ;
                    end
                end
                S_DEF_REQ: state <= S_DEF_WAIT;
                S_DEF_WAIT: if (rd_ack) begin
                    entry <= rd_data;
                    state <= S_WRITE;
                end
                S_WRITE: begin
                    strip_we   <= 1'b1;
                    strip_addr <= strip_base + i;
                    strip_data <= entry;
                    state <= S_NEXT;
                end
                S_NEXT: begin
                    if (i + 16'd1 >= count) begin
                        state <= S_DONE;
                    end else begin
                        i  <= i + 16'd1;
                        if (dir) tx <= tx + 16'd1; else ty <= ty + 16'd1;
                        state <= S_SETUP;
                    end
                end
                S_DONE: begin busy <= 1'b0; state <= S_IDLE; end
                default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
