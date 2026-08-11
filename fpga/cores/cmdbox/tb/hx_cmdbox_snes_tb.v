// hx_cmdbox_snes_tb.v — co-sim of hx_cmdbox_snes.v against the golden model.
// Drives the module like the SNES cart bus: window writes ($3F:F1xx) with a
// wr_end strobe, doorbell/ack, and read-back cycles; also checks `hit` decode
// (asserted only for in-window reads) and out-of-window rejection.
`timescale 1ns/1ps
`default_nettype none

module hx_cmdbox_snes_tb;
    reg clk=0, rst=1;
    always #5 clk=~clk;

    reg [23:0] snes_addr=0;
    reg [7:0]  snes_data=0;
    reg        snes_wr_end=0, snes_read=1, snes_romsel=0;
    wire       hit;
    wire [7:0] rdata;
    reg  [7:0] mcu_addr=0;
    wire [7:0] mcu_rdata;
    wire       pending;
    reg        host_ack=0;

    hx_cmdbox_snes dut (
        .clk(clk), .rst(rst),
        .snes_addr(snes_addr), .snes_data(snes_data),
        .snes_wr_end(snes_wr_end), .snes_read(snes_read), .snes_romsel(snes_romsel),
        .hit(hit), .rdata(rdata),
        .mcu_addr(mcu_addr), .mcu_rdata(mcu_rdata),
        .pending(pending), .host_ack(host_ack)
    );

    reg [7:0] golden [0:31];
    integer gi, errors;

    // a SNES write to $3F:F1<off>: address stable, one-cycle wr_end strobe
    task snes_wr(input [7:0] off, input [7:0] d); begin
        @(posedge clk); snes_read<=1; snes_addr<={8'h3F,8'hF1,off}; snes_data<=d; snes_wr_end<=1;
        @(posedge clk); snes_wr_end<=0;
    end endtask

    // a SNES read of $3F:F1<off>: expect hit + rdata==golden[gi]
    task snes_rd_chk(input [7:0] off); begin
        snes_read=0; snes_addr={8'h3F,8'hF1,off}; #1;
        if (!hit) begin $display("  check %0d: NO HIT at offset %02x", gi, off); errors=errors+1; end
        if (rdata !== golden[gi]) begin
            $display("  check %0d: off=%02x rtl=%02x golden=%02x", gi, off, rdata, golden[gi]);
            errors=errors+1;
        end
        gi=gi+1; snes_read=1;
    end endtask

    // an MCU-bridge read at index <off>: expect mcu_rdata==golden[gi]
    task mcu_rd_chk(input [7:0] off); begin
        mcu_addr=off; #1;
        if (mcu_rdata !== golden[gi]) begin
            $display("  check %0d (MCU): off=%02x rtl=%02x golden=%02x", gi, off, mcu_rdata, golden[gi]);
            errors=errors+1;
        end
        gi=gi+1;
    end endtask

    task host_ack_pulse; begin
        @(posedge clk); host_ack<=1;
        @(posedge clk); host_ack<=0;
    end endtask

    initial begin
        $readmemh("golden.hex", golden);
        errors=0; gi=0;
        repeat (4) @(posedge clk); rst<=0;
        repeat (2) @(posedge clk);

        // Command A block + doorbell
        snes_wr(8'h00,8'h01); snes_wr(8'h01,8'h00); snes_wr(8'h02,8'h0A); snes_wr(8'h03,8'h80);
        snes_wr(8'hFF,8'h01);                       // doorbell

        // the SNES loopback view
        snes_rd_chk(8'hFD);                         // 1: pending
        snes_rd_chk(8'h00); snes_rd_chk(8'h01); snes_rd_chk(8'h02); snes_rd_chk(8'h03); // 2..5

        // the STM32/MCU-bridge view sees the same mailbox
        mcu_rd_chk(8'hFD);                          // 6: pending (MCU)
        mcu_rd_chk(8'h00); mcu_rd_chk(8'h01); mcu_rd_chk(8'h02); mcu_rd_chk(8'h03); // 7..10

        // STM32 consumes it over the bridge -> pending clears
        host_ack_pulse;
        mcu_rd_chk(8'hFD);                          // 11: pending==0 (MCU)

        // out-of-window write (wrong bank $3E) must NOT store, and must not `hit`
        @(posedge clk); snes_read<=1; snes_addr<={8'h3E,8'hF1,8'h00}; snes_data<=8'hFF; snes_wr_end<=1;
        @(posedge clk); snes_wr_end<=0;
        snes_read=0; snes_addr={8'h3E,8'hF1,8'h00}; #1;
        if (hit) begin $display("  HIT asserted out of window!"); errors=errors+1; end
        snes_read=1;
        snes_rd_chk(8'h00);                         // 12: in-window offset 0 still 0x01

        if (errors==0) $display("COSIM PASS: cmdbox_snes window decode + mailbox match golden (%0d checks)", gi);
        else           $display("COSIM FAIL: %0d mismatches", errors);
        $finish;
    end
endmodule

`default_nettype wire
