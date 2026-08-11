// ============================================================
//  hx_mixer_cfg.v — STM32 byte register writes -> hx_mixer_seq cfg-strobe decoder.
//
//  The mixer's per-channel config bus (hx_mixer_seq.v: cfg_we/cfg_ch/cfg_field/
//  cfg_data, fields 0 step_lo,1 step_hi,2 flags,3 vol,4 pan_l,5 pan_r,6 loop_len)
//  wants up to 32 bits per field, but the base MCU bridge's generic register
//  write (opcode FA) carries only ONE byte at a time. So this assembles the wide
//  cfg_data from a low-to-high byte sequence and emits a single cfg strobe when
//  the field's last byte arrives.
//
//  Register index layout (reg_index):  { ch[2:0], field[2:0], bytesel[1:0] }
//    The STM32 writes each field's bytes with bytesel = 0,1,... in order; the
//    commit fires on the field's known last byte:
//      field 0/1 (step lo/hi) : 4 bytes  (bytesel 0..3)
//      field 6   (loop_len)   : 3 bytes  (bytesel 0..2)
//      field 3/4/5 (vol/pan)  : 2 bytes  (bytesel 0..1)
//      field 2   (flags)      : 1 byte   (bytesel 0)
//
//  Pan law stays in software (the STM32 computes vol / pan_l / pan_r); this is a
//  pure byte assembler + router, no arithmetic.
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_mixer_cfg #(
    parameter CHW = 3                 // channel-index width (8 voices -> 3)
)(
    input  wire        clk,
    input  wire        rst,

    // generic register write from the STM32 (base MCU/SPI bridge, opcode FA)
    input  wire        reg_we,        // 1-cycle strobe
    input  wire [7:0]  reg_index,     // { ch[2:0], field[2:0], bytesel[1:0] }
    input  wire [7:0]  reg_value,     // the data byte

    // -> hx_mixer_seq config bus
    output reg         cfg_we,
    output reg  [CHW-1:0] cfg_ch,
    output reg  [2:0]  cfg_field,
    output reg  [31:0] cfg_data
);
    wire [CHW-1:0] w_ch   = reg_index[7:5];
    wire [2:0]     w_field= reg_index[4:2];
    wire [1:0]     w_bsel = reg_index[1:0];

    // last data-byte index per field -> when to emit the cfg strobe
    function [1:0] last_b(input [2:0] f);
        case (f)
            3'd0, 3'd1:        last_b = 2'd3;   // step lo/hi (32-bit)
            3'd6:              last_b = 2'd2;   // loop_len   (24-bit)
            3'd3, 3'd4, 3'd5:  last_b = 2'd1;   // vol/pan    (16-bit)
            default:           last_b = 2'd0;   // flags      (8-bit)
        endcase
    endfunction

    reg [31:0] acc;
    // accumulator with the current byte merged in at its lane
    wire [31:0] acc_next = (w_bsel == 2'd0) ? {acc[31:8],  reg_value}
                         : (w_bsel == 2'd1) ? {acc[31:16], reg_value, acc[7:0]}
                         : (w_bsel == 2'd2) ? {acc[31:24], reg_value, acc[15:0]}
                         :                     {reg_value,  acc[23:0]};

    always @(posedge clk) begin
        cfg_we <= 1'b0;
        if (rst) begin
            acc <= 32'd0;
        end else if (reg_we) begin
            acc <= acc_next;
            if (w_bsel == last_b(w_field)) begin
                cfg_we    <= 1'b1;
                cfg_ch    <= w_ch;
                cfg_field <= w_field;
                cfg_data  <= acc_next;
            end
        end
    end
endmodule

`default_nettype wire
