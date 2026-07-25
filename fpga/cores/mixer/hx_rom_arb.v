`timescale 1ns / 1ps
// ============================================================
//  hx_rom_arb.v — faithful extract of the sd2snes ROM-bus arbiter, plus the
//  HX-421 mixer as a fifth requestor (6b). This mirrors main.v's STATE machine
//  (ST_IDLE -> ST_*_ADDR counts ROM_CYCLE_LEN -> ST_*_END -> ST_IDLE, entered
//  only on free_slot) so the pattern can be simulation-gated against a PSRAM
//  model + a SNES-access generator BEFORE the same additions go into main.v —
//  where a bug crashes every game and can't be scoped on a sealed cart.
//
//  Modelled requestors: MCU read (a representative existing one, to prove the
//  mixer coexists without starving it) + MIX read (the new one). The SNES is the
//  default/lowest-priority path (rom_addr = snes_addr) exactly as in main.v; a
//  requestor overrides it only during its ADDR/END states, which are entered
//  only on free_slot — so no requestor ever drives rom_addr during a SNES access.
//
//  The mixer is the LOWEST-priority requestor (served after MCU), using the same
//  ROM_CYCLE_LEN, so it cannot delay a SNES access or starve the MCU.
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_rom_arb #(
    parameter integer ROM_CYCLE_LEN = 7,
    parameter integer AW = 23
) (
    input  wire          clk,
    input  wire          free_slot,     // ROM bus is free to start a non-SNES access
    input  wire [AW-1:0] snes_addr,     // SNES ROM address (default path)

    // MCU read requestor (representative existing)
    input  wire          mcu_rrq,       // pulse: latch a read of mcu_addr
    input  wire [AW-1:0] mcu_addr,
    output reg  [15:0]   mcu_din,
    output reg           mcu_rdy,       // pulse: mcu_din valid

    // MIXER read requestor (the 6b addition)
    input  wire          mix_rrq,       // pulse: latch a read of mix_addr
    input  wire [AW-1:0] mix_addr,
    output reg  [15:0]   mix_din,
    output reg           mix_rdy,       // pulse: mix_din valid

    // PSRAM (ROM) interface
    output wire [AW-1:0] rom_addr,
    input  wire [15:0]   rom_data
);
    localparam [6:0] S_IDLE  = 7'b0000001,
                     S_MCU_A = 7'b0000010, S_MCU_E = 7'b0000100,
                     S_MIX_A = 7'b0001000, S_MIX_E = 7'b0010000;
    reg [6:0] state = S_IDLE;
    reg [3:0] delay = 0;

    reg              mcu_pend = 0, mix_pend = 0;
    reg [AW-1:0]     mcu_ar = 0, mix_ar = 0;

    wire mcu_hit = state[1] | state[2];   // S_MCU_A | S_MCU_E
    wire mix_hit = state[3] | state[4];   // S_MIX_A | S_MIX_E

    // priority mux — MCU over MIX over the SNES default (mutually exclusive in
    // practice: only one *_hit is set at a time). MIX is lowest.
    assign rom_addr = mcu_hit ? mcu_ar
                    : mix_hit ? mix_ar
                    :           snes_addr;

    // request latches (mirror main.v's per-requestor always blocks)
    always @(posedge clk) begin
        if (mcu_rrq) begin mcu_pend <= 1'b1; mcu_ar <= mcu_addr; end
        else if (state == S_MCU_E) mcu_pend <= 1'b0;
    end
    always @(posedge clk) begin
        if (mix_rrq) begin mix_pend <= 1'b1; mix_ar <= mix_addr; end
        else if (state == S_MIX_E) mix_pend <= 1'b0;
    end

    always @(posedge clk) begin
        mcu_rdy <= 1'b0;
        mix_rdy <= 1'b0;
        case (state)
            S_IDLE: begin
                if (free_slot) begin
                    if (mcu_pend) begin state <= S_MCU_A; delay <= ROM_CYCLE_LEN[3:0]; end
                    else if (mix_pend) begin state <= S_MIX_A; delay <= ROM_CYCLE_LEN[3:0]; end
                end
            end
            S_MCU_A: begin
                delay <= delay - 4'd1;
                mcu_din <= rom_data;                 // settles by delay==0
                if (delay == 0) state <= S_MCU_E;
            end
            S_MIX_A: begin
                delay <= delay - 4'd1;
                mix_din <= rom_data;
                if (delay == 0) state <= S_MIX_E;
            end
            S_MCU_E: begin state <= S_IDLE; mcu_rdy <= 1'b1; end
            S_MIX_E: begin state <= S_IDLE; mix_rdy <= 1'b1; end
            default: state <= S_IDLE;
        endcase
    end

endmodule

`default_nettype wire
