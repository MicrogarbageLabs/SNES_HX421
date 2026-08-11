// ============================================================
//  hx_cmdbox.v — SNES -> cart command mailbox (256 B) + doorbell.
//
//  The RPG's SNES<->FPGA command channel (docs/architecture-pivot.md): the 65816
//  byte-writes a command block (op/slot/asset/gain/pan/...) to this mailbox and
//  then writes the DOORBELL offset to signal "command complete". The STM32 polls
//  `pending`, reads the block back byte-by-byte through the indexed read port, and
//  pulses `ack` to clear the doorbell once it has consumed the command. Same
//  byte-write / indexed-read shape as hx_dsp.v; single clock (the SNES-bus and
//  MCU/SPI clock-domain crossings are handled at the main.v boundary, exactly as
//  snescmd_buf already is).
//
//  WRITE (SNES bus):  w_we + w_addr[7:0] + w_data[7:0]  -> mem[addr]=data;
//                     a write to DOORBELL also raises `pending`.
//  READ  (STM32):     r_addr[7:0] -> r_data[7:0] (async), plus the `pending` flag.
//  ACK   (STM32):     pulse `ack` -> clears `pending`.
//
//  A doorbell write WINS over a same-cycle ack, so a freshly-posted command is
//  never silently dropped by an ack meant for the previous one.
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_cmdbox #(
    parameter [7:0] DOORBELL = 8'hFF
)(
    input  wire        clk,
    input  wire        rst,

    // SNES cart WRITE side (byte-granular stores from the bus)
    input  wire        w_we,
    input  wire [7:0]  w_addr,
    input  wire [7:0]  w_data,

    // READ port 1 (indexed byte readback)
    input  wire [7:0]  r_addr,
    output wire [7:0]  r_data,

    // READ port 2 (independent async read — lets the SNES loopback and the STM32
    // read the mailbox without contending on one address bus)
    input  wire [7:0]  r_addr2,
    output wire [7:0]  r_data2,

    // handshake
    output reg         pending,   // set when the SNES writes DOORBELL
    input  wire        ack        // STM32 pulses to clear pending after consuming
);
    reg [7:0] mem [0:255];
    integer k;
    initial begin
        for (k = 0; k < 256; k = k + 1) mem[k] = 8'd0;
        pending = 1'b0;
    end

    always @(posedge clk) begin
        if (rst) begin
            pending <= 1'b0;
        end else begin
            if (w_we) mem[w_addr] <= w_data;
            // Doorbell-set wins over ack: a new command must not be lost to an
            // ack for the previous one arriving in the same cycle.
            if (w_we && w_addr == DOORBELL) pending <= 1'b1;
            else if (ack)                   pending <= 1'b0;
        end
    end

    // asynchronous indexed readback (small 256 B map -> distributed RAM), two ports
    assign r_data  = mem[r_addr];
    assign r_data2 = mem[r_addr2];
endmodule

`default_nettype wire
