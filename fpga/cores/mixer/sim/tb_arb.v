// ============================================================
//  tb_arb.v — mixer under adversarial PSRAM contention
//
//  hx_audio_top's read port goes through hx_psram_arb, sharing the PSRAM with a
//  synthetic RENDERER that requests every possible cycle (worst-case bus load).
//  Proves the mixer still produces a bit-exact stream with underrun=0 even when
//  the bus is saturated — the arbiter's mixer-priority guarantee. Also reports
//  the mixer's worst-case read latency under contention.
//
//  Public domain (CC0). No warranty.
// ============================================================

`timescale 1ns/1ps
`default_nettype none

module tb_arb;
    localparam N = 8, CHW = 3, STRIDE = 1024, AW = 32;

    reg          clk = 0, rst;
    reg          cfg_we;
    reg  [CHW-1:0] cfg_ch;
    reg  [2:0]   cfg_field;
    reg  [31:0]  cfg_data;
    reg  [3:0]   headroom, out_shift;
    reg  signed [31:0] out_offset, out_min, out_max;
    reg          prime, run;

    // mixer read port
    wire         m_req;
    wire [CHW-1:0] m_ch;
    wire [31:0]  m_off;
    wire         m_ack;
    wire signed [15:0] q_data;
    wire signed [15:0] audio_l, audio_r;
    wire         audio_stb, underrun;

    // renderer port (adversarial: requests every cycle)
    reg          r_req;
    reg  [AW-1:0] r_addr;
    wire         r_ack;

    // PSRAM port (behind the arbiter)
    wire         p_req;
    wire [AW-1:0] p_addr;
    reg          p_ack;
    reg  signed [15:0] p_data;

    integer LATENCY;
    reg signed [15:0] src [0:N*STRIDE-1];

    // mixer subsystem
    hx_audio_top #(.N(N), .CHW(CHW), .TICK_DIV(512)) dut (
        .clk(clk), .rst(rst),
        .cfg_we(cfg_we), .cfg_ch(cfg_ch), .cfg_field(cfg_field), .cfg_data(cfg_data),
        .headroom_bits(headroom), .out_shift(out_shift), .out_offset(out_offset),
        .out_min(out_min), .out_max(out_max),
        .prime(prime), .run(run),
        .rd_req(m_req), .rd_ch(m_ch), .rd_addr(m_off), .rd_ack(m_ack), .rd_data(q_data),
        .audio_l(audio_l), .audio_r(audio_r), .audio_stb(audio_stb), .underrun(underrun)
    );

    // arbiter: mixer's channel offset flattened to an absolute index
    wire [AW-1:0] m_abs = m_ch*STRIDE + m_off;
    hx_psram_arb #(.AW(AW)) arb (
        .clk(clk), .rst(rst),
        .m_req(m_req), .m_addr(m_abs), .m_ack(m_ack),
        .r_req(r_req), .r_addr(r_addr), .r_ack(r_ack),
        .q_data(q_data),
        .p_req(p_req), .p_addr(p_addr), .p_ack(p_ack), .p_data(p_data)
    );

    always #5 clk = ~clk;

    // PSRAM latency model behind the arbiter
    reg pend; integer lat; reg [AW-1:0] hold;
    always @(posedge clk) begin
        p_ack <= 1'b0;
        if (rst) pend <= 0;
        else if (p_req && !pend) begin pend<=1; lat<=LATENCY; hold<=p_addr; end
        else if (pend) begin
            if (lat<=0) begin p_ack<=1; p_data<=(hold<N*STRIDE)?src[hold]:16'sd0; pend<=0; end
            else lat<=lat-1;
        end
    end

    // adversarial renderer: keep a request asserted every cycle
    always @(posedge clk) begin
        if (rst) begin r_req<=0; r_addr<=0; end
        else begin
            r_req <= 1'b1;                 // always wants the bus
            if (r_ack) r_addr <= r_addr + 1;
        end
    end

    // measure mixer read latency (m_req pulse -> m_ack)
    integer mlat, worst_mlat; reg mpend;
    always @(posedge clk) begin
        if (rst) begin mpend<=0; mlat<=0; worst_mlat<=0; end
        else begin
            if (m_req && !mpend) begin mpend<=1; mlat<=0; end
            else if (mpend) begin
                if (m_ack) begin mpend<=0; if (mlat>worst_mlat) worst_mlat<=mlat; end
                else mlat<=mlat+1;
            end
        end
    end

    integer fd, r, i, k, hr, sh, nn, nout, stride, fails, got;
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

        rst=1; cfg_we=0; prime=0; run=0;
        @(posedge clk); #1; @(posedge clk); #1; rst=0;

        r = $fscanf(fd, "HDR %d %d %h %h %h %d %d %d\n", hr, sh, offh, minh, maxh, nn, nout, stride);
        headroom=hr[3:0]; out_shift=sh[3:0]; out_offset=offh; out_min=minh; out_max=maxh;
        for (i=0;i<nn;i=i+1) begin
            r = $fscanf(fd, "CH %d %d %d %d %d %d %h %h %h %h %h\n",
                        ci, ccu, cac, cmu, clp, cll, shi, slo, cvol, cpl, cpr);
            cfgw(ci[CHW-1:0], 3'd0, slo);  cfgw(ci[CHW-1:0], 3'd1, shi);
            cfgw(ci[CHW-1:0], 3'd2, {28'd0, clp[0], cmu[0], cac[0], ccu[0]});
            cfgw(ci[CHW-1:0], 3'd3, {16'd0, cvol}); cfgw(ci[CHW-1:0], 3'd4, {16'd0, cpl});
            cfgw(ci[CHW-1:0], 3'd5, {16'd0, cpr});  cfgw(ci[CHW-1:0], 3'd6, cll[31:0]);
        end
        for (i=0;i<nn;i=i+1) begin
            r = $fscanf(fd, "SRC %d %d\n", sci, snsrc);
            for (k=0;k<snsrc;k=k+1) begin r=$fscanf(fd,"%h\n",sv); src[sci*STRIDE+k]=sv; end
        end
        r = $fscanf(fd, "%s\n", tok);
        for (i=0;i<nout;i=i+1) r = $fscanf(fd, "%h %h\n", exp_l[i], exp_r[i]);
        $fclose(fd);

        prime=1; @(posedge clk); #1; prime=0;
        @(posedge clk); wait (dut.mix_busy==1'b0); #1;

        fails=0; got=0; run=1;
        while (got < nout) begin
            @(posedge clk); #1;
            if (audio_stb) begin
                if (audio_l !== exp_l[got] || audio_r !== exp_r[got]) begin
                    fails = fails + 1;
                    if (fails <= 20)
                        $display("MISMATCH sample %0d: dut(%0d,%0d) exp(%0d,%0d)",
                                 got, $signed(audio_l), $signed(audio_r),
                                 $signed(exp_l[got]), $signed(exp_r[got]));
                end
                got = got + 1;
            end
        end
        run=0;

        $display("hx_arb co-sim: %0d samples, %0d mismatches, underrun=%0d, worst mixer read latency %0d cyc (bus saturated)",
                 got, fails, underrun, worst_mlat);
        if (fails==0 && underrun==0)
            $display("RESULT: PASS - mixer bit-exact + no underrun under full renderer contention");
        else
            $display("RESULT: FAIL");
        $finish;
    end
endmodule

`default_nettype wire
