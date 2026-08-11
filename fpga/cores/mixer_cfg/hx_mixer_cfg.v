// ============================================================
//  hx_mixer_cfg.v — STM32 register write -> hx_mixer_seq cfg-strobe decoder.
//
//  The mixer's per-channel config bus (hx_mixer_seq.v: cfg_we/cfg_ch/cfg_field/
//  cfg_data, fields 0 step_lo,1 step_hi,2 flags,3 vol,4 pan_l,5 pan_r,6 loop_len)
//  is complete and stable, but nothing drives it from a register interface — it
//  is tied off in hx_rpg_top or driven by a hard-coded boot FSM. This is that
//  missing piece: it turns a flat register write from the STM32 (via the base
//  MCU/SPI bridge) into one cfg strobe.
//
//  Register address layout:  reg_addr = { ch[CHW-1:0], field[2:0] }
//    field = reg_addr[2:0], ch = reg_addr[2+CHW:3]. So each channel occupies 8
//    register slots; field 7 is reserved (no write emitted).
//
//  The pan law stays in SOFTWARE (the STM32 computes vol / pan_l / pan_r from its
//  single gain+pan and writes fields 3/4/5), matching the C reference mixer bit-
//  for-bit — this module is a pure router, no arithmetic.
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_mixer_cfg #(
    parameter CHW = 3                 // channel-index width (8 voices -> 3)
)(
    input  wire        clk,
    input  wire        rst,

    // register write from the STM32 (base MCU/SPI bridge presents word writes)
    input  wire        reg_we,
    input  wire [7:0]  reg_addr,      // { ch[CHW-1:0], field[2:0] }
    input  wire [31:0] reg_data,

    // -> hx_mixer_seq config bus
    output reg         cfg_we,
    output reg  [CHW-1:0] cfg_ch,
    output reg  [2:0]  cfg_field,
    output reg  [31:0] cfg_data
);
    wire [2:0]      field = reg_addr[2:0];
    wire [CHW-1:0]  ch    = reg_addr[2+CHW:3];

    always @(posedge clk) begin
        if (rst) begin
            cfg_we <= 1'b0;
        end else begin
            // fields 0..6 are real mixer fields; 7 is reserved -> drop the write
            cfg_we    <= reg_we && (field <= 3'd6);
            cfg_ch    <= ch;
            cfg_field <= field;
            cfg_data  <= reg_data;
        end
    end
endmodule

`default_nettype wire
