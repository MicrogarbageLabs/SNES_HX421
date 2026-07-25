`timescale 1ns / 1ps
// ============================================================
//  tb_rom_arb.v — sim-gate the 6b ROM-bus arbiter change before it touches
//  main.v. A PSRAM model + a SNES-access generator (periodic access windows
//  with free-slot slack) + a continuously-reading mixer + periodic MCU reads.
//
//  Asserts the properties that matter on a sealed cart:
//    SAFETY   — during a SNES access window, rom_addr == snes_addr ALWAYS
//               (no requestor ever drives the bus while the SNES needs it).
//    CORRECT  — every mixer/MCU read returns the PSRAM word at its address.
//    LIVENESS — the mixer is served often, and the MCU is never starved.
// ============================================================
`default_nettype none

module tb_rom_arb;
    localparam AW = 23;
    localparam CYC = 7;

    reg clk = 0;
    always #5 clk = ~clk;

    // ---- PSRAM model: combinational read, known pattern ----
    function [15:0] psram; input [AW-1:0] a;
        psram = a[15:0] ^ 16'h5A3C ^ {a[22:16], 9'd0};
    endfunction
    wire [AW-1:0] rom_addr;
    wire [15:0]   rom_data = psram(rom_addr);

    // ---- SNES access generator ----
    // period 27 cyc; access window = first 4 cyc (SNES needs the bus); free_slot
    // pulses at cyc 5 (start of the idle window, >= CYC+margin slack).
    localparam PERIOD = 27, ACCESS = 4, FREEAT = 5;
    reg [7:0] phase = 0;
    reg [AW-1:0] snes_ar = 23'h400000;
    wire snes_active = (phase < ACCESS);
    wire free_slot   = (phase == FREEAT);
    always @(posedge clk) begin
        if (phase == PERIOD-1) begin phase <= 0; snes_ar <= snes_ar + 23'h11; end
        else phase <= phase + 8'd1;
    end

    // ---- requestors ----
    reg          mcu_rrq=0, mix_rrq=0;
    reg  [AW-1:0] mcu_addr=0, mix_addr=0;
    wire [15:0]  mcu_din, mix_din;
    wire         mcu_rdy, mix_rdy;

    hx_rom_arb #(.ROM_CYCLE_LEN(CYC), .AW(AW)) dut (
        .clk(clk), .free_slot(free_slot), .snes_addr(snes_ar),
        .mcu_rrq(mcu_rrq), .mcu_addr(mcu_addr), .mcu_din(mcu_din), .mcu_rdy(mcu_rdy),
        .mix_rrq(mix_rrq), .mix_addr(mix_addr), .mix_din(mix_din), .mix_rdy(mix_rdy),
        .rom_addr(rom_addr), .rom_data(rom_data)
    );

    // ---- mixer model: continuously reads a stream of addresses ----
    reg mix_busy = 0;
    reg [AW-1:0] mix_seq = 23'h000100, mix_expect_a = 0;
    integer mix_reads = 0, mcu_reads = 0, fail = 0;
    reg [15:0] samp;
    always @(posedge clk) begin
        mix_rrq <= 1'b0;
        if (!mix_busy) begin
            mix_addr <= mix_seq; mix_rrq <= 1'b1; mix_expect_a <= mix_seq;
            mix_busy <= 1'b1;
        end else if (mix_rdy) begin
            if (mix_din !== psram(mix_expect_a)) begin
                $display("FAIL: mixer read %06x got %04x exp %04x", mix_expect_a, mix_din, psram(mix_expect_a));
                fail = 1;
            end
            mix_reads = mix_reads + 1;
            mix_seq <= mix_seq + 23'h2;    // next sample
            mix_busy <= 1'b0;
        end
    end

    // ---- MCU model: an occasional read ----
    reg mcu_busy = 0;
    reg [AW-1:0] mcu_seq = 23'h200000, mcu_expect_a = 0;
    reg [11:0] mcu_gap = 0;
    always @(posedge clk) begin
        mcu_rrq <= 1'b0;
        if (!mcu_busy) begin
            mcu_gap <= mcu_gap + 12'd1;
            if (mcu_gap == 12'd200) begin
                mcu_gap <= 0;
                mcu_addr <= mcu_seq; mcu_rrq <= 1'b1; mcu_expect_a <= mcu_seq;
                mcu_busy <= 1'b1;
            end
        end else if (mcu_rdy) begin
            if (mcu_din !== psram(mcu_expect_a)) begin
                $display("FAIL: MCU read %06x got %04x exp %04x", mcu_expect_a, mcu_din, psram(mcu_expect_a));
                fail = 1;
            end
            mcu_reads = mcu_reads + 1;
            mcu_seq <= mcu_seq + 23'h100;
            mcu_busy <= 1'b0;
        end
    end

    // ---- SAFETY: during a SNES access, the bus must carry the SNES address ----
    integer safety_checks = 0;
    always @(posedge clk) begin
        if (snes_active) begin
            safety_checks = safety_checks + 1;
            if (rom_addr !== snes_ar) begin
                $display("FAIL: SAFETY at phase %0d — rom_addr %06x != snes_addr %06x (a requestor stole the bus)",
                         phase, rom_addr, snes_ar);
                fail = 1;
            end
        end
    end

    initial begin
        #400000;   // ~40k cycles
        $display("rom-arb: mixer reads=%0d  MCU reads=%0d  safety checks=%0d", mix_reads, mcu_reads, safety_checks);
        if (mix_reads < 1000) begin $display("FAIL: mixer starved (%0d reads)", mix_reads); fail = 1; end
        if (mcu_reads < 100)  begin $display("FAIL: MCU starved (%0d reads)", mcu_reads); fail = 1; end
        if (fail) $display("RESULT: FAIL - 6b arbiter unsafe or incorrect");
        else      $display("RESULT: PASS - mixer reads PSRAM in free slots; SNES never delayed; MCU not starved");
        $finish;
    end
endmodule

`default_nettype wire
