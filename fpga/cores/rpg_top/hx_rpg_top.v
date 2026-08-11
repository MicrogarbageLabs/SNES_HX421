// ============================================================
//  hx_rpg_top.v — integrated RPG core: all five engines on one fabric
//
//  Wires the five measured engines together with the integration logic the
//  per-module standalone fits did NOT include: a PSRAM read arbiter shared by
//  the PSRAM readers (mixer, and strip/FMV muxed since they never run at once —
//  gameplay vs cutscene) and a shared staging BRAM with write arbitration among
//  the four writers (strip, actor, FMV, scene). This is the combined-fit harness:
//  it measures the whole core (5 engines + arbiter + staging + glue) on the
//  EP4CE15; it is not functionally exact. See docs/architecture-pivot.md.
// ============================================================

`default_nettype none

module hx_rpg_top (
    input  wire        clk,
    input  wire        rst,

    input  wire        s_cfg_we, input wire [1:0] s_cfg_layer, input wire [4:0] s_cfg_field,
    input  wire [31:0] s_cfg_data,
    input  wire        s_go, input wire [1:0] s_go_layer,
    input  wire signed [15:0] s_go_x, input wire signed [15:0] s_go_y,

    input  wire        a_cfg_we, input wire [2:0] a_cfg_field, input wire [31:0] a_cfg_data,
    input  wire        a_act_we, input wire [6:0] a_act_idx, input wire [63:0] a_act_data,
    input  wire        a_go,

    input  wire        sc_reg_we, input wire [3:0] sc_reg_idx, input wire sc_reg_is16,
    input  wire [15:0] sc_reg_addr, input wire [15:0] sc_reg_val, input wire [4:0] sc_reg_count,
    input  wire [5:0]  sc_desc_count, input wire sc_go,

    input  wire        f_cfg_we, input wire [3:0] f_cfg_field, input wire [31:0] f_cfg_data,
    input  wire        f_go, input wire [2:0] f_go_sub, input wire f_go_parity,

    input  wire        m_cfg_we, input wire [2:0] m_cfg_ch, input wire [2:0] m_cfg_field,
    input  wire [31:0] m_cfg_data,
    input  wire [3:0]  m_headroom, input wire [3:0] m_out_shift,
    input  wire signed [31:0] m_out_offset, input wire signed [31:0] m_out_min,
    input  wire signed [31:0] m_out_max, input wire m_prime, input wire m_run,

    input  wire [11:0] stage_rd,
    output reg  [15:0] stage_q,

    output wire        psram_req,
    output wire [31:0] psram_addr,
    input  wire        psram_ack,
    input  wire signed [15:0] psram_data,

    output wire        busy_any,
    output wire signed [15:0] audio_l,
    output wire signed [15:0] audio_r,
    output wire        audio_stb,
    output wire [2:0]  mix_rd_ch,

    // addressed per-channel mixer drain pointer (STM32 reads it via the base
    // MCU/SPI bridge to pace SD refills against real consumption)
    input  wire [2:0]  m_pos_sel,
    output wire [23:0] m_pos_out
);
    // ---- inter-module nets (declared up front for default_nettype none) ----
    wire        s_busy, s_rd_req, s_rd_ack, s_strip_we;
    wire [31:0] s_rd_addr;
    wire [15:0] s_strip_addr, s_strip_data;

    wire        a_busy, a_oam_we;
    wire [9:0]  a_oam_addr;
    wire [7:0]  a_oam_data;

    wire        f_busy, f_rd_req, f_rd_ack, f_st_we, f_desc_we;
    wire [31:0] f_rd_addr;
    wire [15:0] f_st_addr, f_st_data, f_desc_src, f_desc_size, f_desc_vdst;
    wire [7:0]  f_desc_vmain;

    wire        sc_busy, sc_code_we;
    wire [15:0] sc_code_addr;
    wire [7:0]  sc_code_data;

    wire        mix_rd_req, mix_rd_ack, mix_underrun;
    wire [31:0] mix_rd_addr;
    wire signed [15:0] mix_rd_data;

    wire        rend_req, rend_ack;
    wire [31:0] rend_addr;
    wire signed [15:0] rend_data;

    reg  [4:0]  desc_idx;

    // ---- engines ----
    hx_strip u_strip (
        .clk(clk), .rst(rst),
        .cfg_we(s_cfg_we), .cfg_layer(s_cfg_layer), .cfg_field(s_cfg_field), .cfg_data(s_cfg_data),
        .go(s_go), .go_layer(s_go_layer), .go_x(s_go_x), .go_y(s_go_y), .busy(s_busy),
        .rd_req(s_rd_req), .rd_addr(s_rd_addr), .rd_ack(s_rd_ack), .rd_data(rend_data[15:0]),
        .strip_we(s_strip_we), .strip_addr(s_strip_addr), .strip_data(s_strip_data)
    );

    hx_actor u_actor (
        .clk(clk), .rst(rst),
        .cfg_we(a_cfg_we), .cfg_field(a_cfg_field), .cfg_data(a_cfg_data),
        .act_we(a_act_we), .act_idx(a_act_idx), .act_data(a_act_data),
        .go(a_go), .busy(a_busy),
        .oam_we(a_oam_we), .oam_addr(a_oam_addr), .oam_data(a_oam_data)
    );

    hx_fmv u_fmv (
        .clk(clk), .rst(rst),
        .cfg_we(f_cfg_we), .cfg_field(f_cfg_field), .cfg_data(f_cfg_data),
        .go(f_go), .go_sub(f_go_sub), .go_parity(f_go_parity), .busy(f_busy),
        .rd_req(f_rd_req), .rd_addr(f_rd_addr), .rd_ack(f_rd_ack), .rd_data(rend_data[15:0]),
        .st_we(f_st_we), .st_addr(f_st_addr), .st_data(f_st_data),
        .desc_we(f_desc_we), .desc_src(f_desc_src), .desc_size(f_desc_size),
        .desc_vdst(f_desc_vdst), .desc_vmain(f_desc_vmain)
    );

    hx_scene u_scene (
        .clk(clk), .rst(rst),
        .reg_we(sc_reg_we), .reg_idx(sc_reg_idx), .reg_is16(sc_reg_is16),
        .reg_addr(sc_reg_addr), .reg_val(sc_reg_val), .reg_count(sc_reg_count),
        .desc_we(f_desc_we), .desc_idx(desc_idx), .desc_src(f_desc_src), .desc_size(f_desc_size),
        .desc_vdst(f_desc_vdst), .desc_vmain(f_desc_vmain), .desc_count(sc_desc_count),
        .go(sc_go), .busy(sc_busy),
        .code_we(sc_code_we), .code_addr(sc_code_addr), .code_data(sc_code_data)
    );

    hx_audio_top u_mixer (
        .clk(clk), .rst(rst),
        .cfg_we(m_cfg_we), .cfg_ch(m_cfg_ch), .cfg_field(m_cfg_field), .cfg_data(m_cfg_data),
        .headroom_bits(m_headroom), .out_shift(m_out_shift), .out_offset(m_out_offset),
        .out_min(m_out_min), .out_max(m_out_max), .prime(m_prime), .run(m_run),
        .rd_req(mix_rd_req), .rd_ch(mix_rd_ch), .rd_addr(mix_rd_addr),
        .rd_ack(mix_rd_ack), .rd_data(mix_rd_data),
        .audio_l(audio_l), .audio_r(audio_r), .audio_stb(audio_stb), .underrun(mix_underrun),
        .pos_sel(m_pos_sel), .pos_out(m_pos_out)
    );

    hx_psram_arb #(.AW(32)) u_arb (
        .clk(clk), .rst(rst),
        .m_req(mix_rd_req), .m_addr(mix_rd_addr), .m_ack(mix_rd_ack),
        .r_req(rend_req),   .r_addr(rend_addr),   .r_ack(rend_ack),
        .q_data(rend_data),
        .p_req(psram_req), .p_addr(psram_addr), .p_ack(psram_ack), .p_data(psram_data)
    );

    // ---- renderer port: strip and FMV share it (never simultaneous) ----
    assign rend_req  = s_rd_req | f_rd_req;
    assign rend_addr = s_rd_req ? s_rd_addr : f_rd_addr;
    assign s_rd_ack  = rend_ack & s_rd_req;
    assign f_rd_ack  = rend_ack & f_rd_req;
    assign mix_rd_data = rend_data;      // shared return latch

    // ---- scene descriptor index counter (fed by FMV descriptors) ----
    always @(posedge clk) if (rst) desc_idx<=5'd0; else if (f_desc_we) desc_idx<=desc_idx+5'd1;

    // ---- shared staging BRAM (4096 words) + write arbitration ----
    reg [15:0] stage [0:4095];
    always @(posedge clk) begin
        if      (s_strip_we) stage[s_strip_addr[11:0]] <= s_strip_data;
        else if (f_st_we)    stage[f_st_addr[11:0]]    <= f_st_data;
        else if (a_oam_we)   stage[{2'b00,a_oam_addr}] <= {8'd0, a_oam_data};
        else if (sc_code_we) stage[sc_code_addr[11:0]] <= {8'd0, sc_code_data};
        stage_q <= stage[stage_rd];
    end

    assign busy_any = s_busy | a_busy | sc_busy | f_busy;
endmodule

`default_nettype wire
