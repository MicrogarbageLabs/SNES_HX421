// ============================================================
//  hx_psram_arb.v — priority read arbiter for the shared PSRAM port
//
//  Two requestors share one PSRAM read port. The MIXER is highest priority
//  because it has a hard 22.7 us deadline; the RENDERER (and by extension the
//  tilemap / MCU, folded into one bursty low-priority port here) takes the rest.
//  The bandwidth analysis said the mixer uses ~5% of the port, so giving it top
//  priority cannot starve the renderer — but it bounds the mixer's worst-case
//  read latency to (one in-flight transaction) + (its own), which the
//  latency-tolerant mixer absorbs.
//
//  One transaction at a time (async PSRAM, no pipelining). A grant is not
//  preempted mid-transaction — the mixer waits at most the current renderer read
//  (~7 cycles) before being served.
//
//  Handshake per port: req + addr in, ack + shared data out (latch on ack).
//
//  Public domain (CC0). No warranty.
// ============================================================

`default_nettype none

module hx_psram_arb #(
    parameter integer AW = 32
) (
    input  wire        clk,
    input  wire        rst,

    // mixer port (high priority)
    input  wire        m_req,
    input  wire [AW-1:0] m_addr,
    output reg         m_ack,

    // renderer port (low priority)
    input  wire        r_req,
    input  wire [AW-1:0] r_addr,
    output reg         r_ack,

    // shared data returned to whichever port was granted (latch on its ack)
    output reg  signed [15:0] q_data,

    // PSRAM port (to the memory model / real controller)
    output reg         p_req,
    output reg  [AW-1:0] p_addr,
    input  wire        p_ack,
    input  wire signed [15:0] p_data
);
    localparam A_IDLE=2'd0, A_ISSUE=2'd1, A_WAIT=2'd2;
    reg [1:0] astate;
    reg       grant_m;              // 1 = current grant is the mixer, 0 = renderer

    always @(posedge clk) begin
        m_ack <= 1'b0;
        r_ack <= 1'b0;
        p_req <= 1'b0;
        if (rst) begin
            astate <= A_IDLE; grant_m <= 0;
        end else begin
            case (astate)
                A_IDLE: begin
                    if (m_req) begin           // mixer wins ties -> bounded latency
                        grant_m <= 1'b1; p_addr <= m_addr; astate <= A_ISSUE;
                    end else if (r_req) begin
                        grant_m <= 1'b0; p_addr <= r_addr; astate <= A_ISSUE;
                    end
                end
                A_ISSUE: begin
                    p_req  <= 1'b1;            // p_addr held from A_IDLE
                    astate <= A_WAIT;
                end
                A_WAIT: begin
                    if (p_ack) begin
                        q_data <= p_data;
                        if (grant_m) m_ack <= 1'b1;
                        else         r_ack <= 1'b1;
                        astate <= A_IDLE;
                    end
                end
            endcase
        end
    end

endmodule

`default_nettype wire
