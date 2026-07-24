// ============================================================
//  tb_mix.v — co-sim for hx_mixer.v against a full mixer_render scene
//
//  Reads mix_vectors.txt: configures the DUT's 8 channels, loads per-channel
//  source into one memory (channel i at i*STRIDE), primes, renders NOUT frames,
//  and compares each finalized stereo frame to what mixer_render emitted.
//
//  Public domain (CC0). No warranty.
// ============================================================

`timescale 1ns/1ps
`default_nettype none

module tb_mix;
    localparam N = 8, CHW = 3, STRIDE = 512;

    reg          clk = 0, rst;
    reg          cfg_we;
    reg  [CHW-1:0] cfg_ch;
    reg  [2:0]   cfg_field;
    reg  [31:0]  cfg_data;
    reg  [3:0]   headroom, out_shift;
    reg  signed [31:0] out_offset, out_min, out_max;
    reg          start, render;
    wire [CHW-1:0] rd_ch;
    wire [31:0]  rd_base;
    wire signed [31:0] out_l, out_r;
    wire         out_valid;

    // one source memory, channel i based at i*STRIDE. Combinational 4-wide
    // read, 0 past the end (matches the C's peek-past-end -> silence).
    reg signed [15:0] src [0:N*STRIDE-1];
    wire [31:0] a0 = rd_ch*STRIDE + rd_base;
    wire signed [15:0] rd_s0, rd_s1, rd_s2, rd_s3;
    assign rd_s0 = (a0     < N*STRIDE) ? src[a0]     : 16'sd0;
    assign rd_s1 = (a0+1   < N*STRIDE) ? src[a0+1]   : 16'sd0;
    assign rd_s2 = (a0+2   < N*STRIDE) ? src[a0+2]   : 16'sd0;
    assign rd_s3 = (a0+3   < N*STRIDE) ? src[a0+3]   : 16'sd0;

    hx_mixer #(.N(N), .CHW(CHW)) dut (
        .clk(clk), .rst(rst),
        .cfg_we(cfg_we), .cfg_ch(cfg_ch), .cfg_field(cfg_field), .cfg_data(cfg_data),
        .headroom_bits(headroom), .out_shift(out_shift), .out_offset(out_offset),
        .out_min(out_min), .out_max(out_max),
        .start(start), .render(render),
        .rd_ch(rd_ch), .rd_base(rd_base),
        .rd_s0(rd_s0), .rd_s1(rd_s1), .rd_s2(rd_s2), .rd_s3(rd_s3),
        .out_l(out_l), .out_r(out_r), .out_valid(out_valid)
    );

    always #5 clk = ~clk;

    integer fd, r, i, k, hr, sh, nn, nout, stride, fails;
    integer ci, ccu, cac, cmu, sci, snsrc;
    reg [31:0] shi, slo, offh, minh, maxh;
    reg [15:0] cvol, cpl, cpr, sv;
    reg [15:0] exp_l [0:1023];
    reg [15:0] exp_r [0:1023];
    reg [127:0] tok;

    task cfgw(input [CHW-1:0] ch, input [2:0] fld, input [31:0] d);
        begin cfg_ch=ch; cfg_field=fld; cfg_data=d; cfg_we=1; @(posedge clk); #1; cfg_we=0; end
    endtask

    initial begin
        fd = $fopen("mix_vectors.txt","r");
        if (fd==0) begin $display("FATAL: no mix_vectors.txt"); $finish; end

        rst=1; cfg_we=0; start=0; render=0;
        @(posedge clk); #1; @(posedge clk); #1; rst=0;

        // HDR headroom out_shift offset min max N NOUT STRIDE
        r = $fscanf(fd, "HDR %d %d %h %h %h %d %d %d\n", hr, sh, offh, minh, maxh, nn, nout, stride);
        headroom = hr[3:0]; out_shift = sh[3:0];
        out_offset = offh; out_min = minh; out_max = maxh;

        // N channel config lines
        for (i=0;i<nn;i=i+1) begin
            r = $fscanf(fd, "CH %d %d %d %d %h %h %h %h %h\n",
                        ci, ccu, cac, cmu, shi, slo, cvol, cpl, cpr);
            cfgw(ci[CHW-1:0], 3'd0, slo);
            cfgw(ci[CHW-1:0], 3'd1, shi);
            cfgw(ci[CHW-1:0], 3'd2, {29'd0, cmu[0], cac[0], ccu[0]});
            cfgw(ci[CHW-1:0], 3'd3, {16'd0, cvol});
            cfgw(ci[CHW-1:0], 3'd4, {16'd0, cpl});
            cfgw(ci[CHW-1:0], 3'd5, {16'd0, cpr});
        end

        // per-channel source
        for (i=0;i<nn;i=i+1) begin
            r = $fscanf(fd, "SRC %d %d\n", sci, snsrc);
            for (k=0;k<snsrc;k=k+1) begin
                r = $fscanf(fd, "%h\n", sv);
                src[sci*STRIDE + k] = sv;
            end
        end

        // OUT marker then NOUT frames
        r = $fscanf(fd, "%s\n", tok);   // "OUT"
        for (i=0;i<nout;i=i+1) begin
            r = $fscanf(fd, "%h %h\n", exp_l[i], exp_r[i]);
        end
        $fclose(fd);

        // prime all channels
        start=1; @(posedge clk); #1; start=0;
        // PRIME runs N cycles; wait for it to finish (state back to IDLE)
        repeat (nn+2) @(posedge clk);
        #1;

        // render NOUT frames
        fails = 0;
        for (k=0;k<nout;k=k+1) begin
            render=1; @(posedge clk); #1; render=0;
            // sequencer: PROD (N cycles) then FINAL; wait for out_valid
            wait (out_valid == 1'b1);
            #1;
            if (out_l[15:0] !== exp_l[k] || out_r[15:0] !== exp_r[k]) begin
                fails = fails + 1;
                if (fails <= 20)
                    $display("MISMATCH frame %0d: dut(%0d,%0d) exp(%0d,%0d)",
                             k, $signed(out_l[15:0]), $signed(out_r[15:0]),
                             $signed(exp_l[k]), $signed(exp_r[k]));
            end
            @(posedge clk); #1;   // let sequencer return to IDLE before next render
        end

        $display("hx_mixer co-sim: %0d frames, %0d mismatches", nout, fails);
        if (fails==0) $display("RESULT: PASS - 8-channel render bit-exact to mixer_render");
        else          $display("RESULT: FAIL");
        $finish;
    end
endmodule

`default_nettype wire
