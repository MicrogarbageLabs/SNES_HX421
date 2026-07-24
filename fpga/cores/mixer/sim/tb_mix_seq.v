// ============================================================
//  tb_mix_seq.v — co-sim for hx_mixer_seq.v under modelled PSRAM latency
//
//  Replays the SAME golden scene from mix_vectors.txt (the real mixer_render
//  output) through the latency-tolerant engine, with a PSRAM model that answers
//  each read LATENCY cycles late. Output must be bit-identical to the golden and
//  to hx_mixer — latency changes timing, not values. LATENCY is a plusarg so one
//  build can be swept over 1 / 7 / 12 cycles.
//
//  Public domain (CC0). No warranty.
// ============================================================

`timescale 1ns/1ps
`default_nettype none

module tb_mix_seq;
    localparam N = 8, CHW = 3, STRIDE = 1024;

    reg          clk = 0, rst;
    reg          cfg_we;
    reg  [CHW-1:0] cfg_ch;
    reg  [2:0]   cfg_field;
    reg  [31:0]  cfg_data;
    reg  [3:0]   headroom, out_shift;
    reg  signed [31:0] out_offset, out_min, out_max;
    reg          start, render;

    wire         rd_req;
    wire [CHW-1:0] rd_ch;
    wire [31:0]  rd_addr;
    reg          rd_ack;
    reg  signed [15:0] rd_data;
    wire signed [31:0] out_l, out_r;
    wire         out_valid, busy;

    reg signed [15:0] src [0:N*STRIDE-1];

    hx_mixer_seq #(.N(N), .CHW(CHW)) dut (
        .clk(clk), .rst(rst),
        .cfg_we(cfg_we), .cfg_ch(cfg_ch), .cfg_field(cfg_field), .cfg_data(cfg_data),
        .headroom_bits(headroom), .out_shift(out_shift), .out_offset(out_offset),
        .out_min(out_min), .out_max(out_max),
        .start(start), .render(render),
        .rd_req(rd_req), .rd_ch(rd_ch), .rd_addr(rd_addr), .rd_ack(rd_ack), .rd_data(rd_data),
        .out_l(out_l), .out_r(out_r), .out_valid(out_valid), .busy(busy)
    );

    always #5 clk = ~clk;

    // ---- PSRAM latency model: answer each request LATENCY cycles late ----
    integer LATENCY;
    reg        pend;
    integer    lat;
    reg [31:0] hold_idx;
    always @(posedge clk) begin
        rd_ack <= 1'b0;
        if (rst) begin pend <= 0; end
        else if (rd_req && !pend) begin
            pend <= 1; lat <= LATENCY;
            hold_idx <= rd_ch*STRIDE + rd_addr;
        end else if (pend) begin
            if (lat <= 0) begin
                rd_ack  <= 1'b1;
                rd_data <= (hold_idx < N*STRIDE) ? src[hold_idx] : 16'sd0;
                pend    <= 0;
            end else lat <= lat - 1;
        end
    end

    integer fd, r, i, k, hr, sh, nn, nout, stride, fails, cyc, worst_cyc;
    integer ci, ccu, cac, cmu, clp, cll, sci, snsrc;
    reg [31:0] shi, slo, offh, minh, maxh;
    reg [15:0] cvol, cpl, cpr, sv;
    reg [15:0] exp_l [0:1023];
    reg [15:0] exp_r [0:1023];
    reg [127:0] tok;

    task cfgw(input [CHW-1:0] ch, input [2:0] fld, input [31:0] d);
        begin cfg_ch=ch; cfg_field=fld; cfg_data=d; cfg_we=1; @(posedge clk); #1; cfg_we=0; end
    endtask

    initial begin
        if (!$value$plusargs("LAT=%d", LATENCY)) LATENCY = 7;

        fd = $fopen("mix_vectors.txt","r");
        if (fd==0) begin $display("FATAL: no mix_vectors.txt"); $finish; end

        rst=1; cfg_we=0; start=0; render=0;
        @(posedge clk); #1; @(posedge clk); #1; rst=0;

        r = $fscanf(fd, "HDR %d %d %h %h %h %d %d %d\n", hr, sh, offh, minh, maxh, nn, nout, stride);
        headroom=hr[3:0]; out_shift=sh[3:0]; out_offset=offh; out_min=minh; out_max=maxh;

        for (i=0;i<nn;i=i+1) begin
            r = $fscanf(fd, "CH %d %d %d %d %d %d %h %h %h %h %h\n",
                        ci, ccu, cac, cmu, clp, cll, shi, slo, cvol, cpl, cpr);
            cfgw(ci[CHW-1:0], 3'd0, slo);
            cfgw(ci[CHW-1:0], 3'd1, shi);
            cfgw(ci[CHW-1:0], 3'd2, {28'd0, clp[0], cmu[0], cac[0], ccu[0]});
            cfgw(ci[CHW-1:0], 3'd3, {16'd0, cvol});
            cfgw(ci[CHW-1:0], 3'd4, {16'd0, cpl});
            cfgw(ci[CHW-1:0], 3'd5, {16'd0, cpr});
            cfgw(ci[CHW-1:0], 3'd6, cll[31:0]);
        end
        for (i=0;i<nn;i=i+1) begin
            r = $fscanf(fd, "SRC %d %d\n", sci, snsrc);
            for (k=0;k<snsrc;k=k+1) begin r=$fscanf(fd,"%h\n",sv); src[sci*STRIDE+k]=sv; end
        end
        r = $fscanf(fd, "%s\n", tok);
        for (i=0;i<nout;i=i+1) r = $fscanf(fd, "%h %h\n", exp_l[i], exp_r[i]);
        $fclose(fd);

        // prime, wait for the sequencer to finish
        start=1; @(posedge clk); #1; start=0;
        @(posedge clk); wait (busy==1'b0); #1;

        fails = 0; worst_cyc = 0;
        for (k=0;k<nout;k=k+1) begin
            render=1; cyc=0; @(posedge clk); #1; render=0;
            while (out_valid !== 1'b1) begin @(posedge clk); #1; cyc=cyc+1; end
            if (cyc > worst_cyc) worst_cyc = cyc;
            if (out_l[15:0] !== exp_l[k] || out_r[15:0] !== exp_r[k]) begin
                fails = fails + 1;
                if (fails <= 20)
                    $display("MISMATCH frame %0d: dut(%0d,%0d) exp(%0d,%0d)",
                             k, $signed(out_l[15:0]), $signed(out_r[15:0]),
                             $signed(exp_l[k]), $signed(exp_r[k]));
            end
            @(posedge clk); wait (busy==1'b0); #1;   // back to IDLE before next
        end

        // 96 MHz clock, 22.7 us sample period => 2177 cycles/frame available.
        $display("hx_mixer_seq co-sim (latency=%0d): %0d frames, %0d mismatches, worst %0d cyc/frame (of 2177 @96MHz/44.1kHz)",
                 LATENCY, nout, fails, worst_cyc);
        if (fails==0) $display("RESULT: PASS - latency-tolerant render bit-exact to mixer_render");
        else          $display("RESULT: FAIL");
        $finish;
    end
endmodule

`default_nettype wire
