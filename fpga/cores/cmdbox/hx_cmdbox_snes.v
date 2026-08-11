// ============================================================
//  hx_cmdbox_snes.v — hx_cmdbox wrapped with the SNES-bus address decode, so
//  main.v just instantiates this and adds one read-mux term. Keeping the decode
//  here (not inline in main.v) makes it co-sim-verifiable in isolation — main.v
//  itself can't be simulated (Altera megafunctions).
//
//  Window: a 256-byte region at {WIN_BANK, WIN_HI, offset}. The 65816 byte-writes
//  a command block into offsets 0.., then writes DOORBELL (0xFF) to post it; the
//  host (STM32) reads it back over the MCU bridge and pulses host_ack. For
//  hardware bring-up WITHOUT the STM32, the SNES can also read the window back
//  (loopback) and self-ack by writing ACK_OFF — proving the write decode + BRAM +
//  doorbell on silicon with the SNES alone.
//
//  Offsets:  0x00..(cmd bytes)   0xFD read = {7'b0, pending} (status)
//            0xFE write = ack (bring-up)   0xFF write = doorbell (posts command)
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_cmdbox_snes #(
    parameter [7:0] WIN_BANK = 8'h3F,   // SNES bank carrying the window
    parameter [7:0] WIN_HI   = 8'hF1,   // high address byte (window = $BANK:HI00..HIFF)
    parameter [7:0] ACK_OFF  = 8'hFE,   // SNES write here self-acks (bring-up)
    parameter [7:0] PEND_OFF = 8'hFD    // SNES read here returns pending status
)(
    input  wire        clk,
    input  wire        rst,

    // SNES bus
    input  wire [23:0] snes_addr,
    input  wire [7:0]  snes_data,       // bus write data (valid at snes_wr_end)
    input  wire        snes_wr_end,     // 1-cycle strobe at write completion
    input  wire        snes_read,       // 0 = read cycle active
    input  wire        snes_romsel,     // 0 = cart selected

    // SNES read output (feed main.v's SNES_DATA read mux)
    output wire        hit,             // window is being read this cycle
    output wire [7:0]  rdata,

    // host (STM32) side — read over the MCU bridge (F9 <group> <index>) + ack
    input  wire [7:0]  mcu_addr,        // MCU read index (= reg_read/index_read_buf)
    output wire [7:0]  mcu_rdata,       // -> mcu_cmd mbox_mcu_rdata
    output wire        pending,         // a command is posted and unconsumed
    input  wire        host_ack         // STM32 pulses to consume (OR'd with SNES self-ack)
);
    wire win = ~snes_romsel
             & (snes_addr[23:16] == WIN_BANK)
             & (snes_addr[15:8]  == WIN_HI);

    wire we       = snes_wr_end & win;                     // any write in the window
    wire snes_ack = we & (snes_addr[7:0] == ACK_OFF);      // bring-up self-ack

    wire [7:0] mb_rdata, mb_rdata2;
    hx_cmdbox u_box (
        .clk(clk), .rst(rst),
        .w_we(we), .w_addr(snes_addr[7:0]), .w_data(snes_data),
        .r_addr(snes_addr[7:0]), .r_data(mb_rdata),      // port 1: SNES loopback
        .r_addr2(mcu_addr),      .r_data2(mb_rdata2),     // port 2: STM32 (MCU bridge)
        .pending(pending), .ack(snes_ack | host_ack)
    );

    assign hit   = ~snes_read & win;
    // both read views expose pending at PEND_OFF, mailbox bytes elsewhere
    assign rdata     = (snes_addr[7:0] == PEND_OFF) ? {7'b0, pending} : mb_rdata;
    assign mcu_rdata = (mcu_addr       == PEND_OFF) ? {7'b0, pending} : mb_rdata2;
endmodule

`default_nettype wire
