// ============================================================
//  hx_dsp.v — mailbox math coprocessor (DSP function service)
//
//  The DSP-1/CX4 pattern: the 65816 can't multiply/divide fast, so it byte-writes
//  a function selector + operands to the cart WRITE mailbox, NOPs a fixed latency,
//  then reads the result bytes back by index via the READ mailbox. No handshake
//  needed — the latency is deterministic per function — but a `ready` byte is also
//  exposed for robustness. Uses the FPGA's spare DSP blocks (mixer uses 24/112).
//
//  WRITE map (w_addr):  0=FUNC  1..4=ARGA byte0..3  5..8=ARGB byte0..3  15=START
//  READ  map (r_addr):  0..3=RESULT byte0..3        4=STATUS{bit0=ready}
//  FUNC:  0 MUL (signed 16x16->32, result=A*B)      1 MAC   (acc += A*B; result=acc)
//         2 DIV (unsigned A[31:0]/B[15:0]->Q)        3 MACINIT (acc = A*B; result=acc)
//
//  Latencies (cycles from START to ready): MUL/MAC/MACINIT 1, DIV ~33. Signed
//  16x16 for MUL/MAC (one DSP block); DIV is iterative restoring division.
//  RESOURCE-REPRESENTATIVE starter set. CC0.
// ============================================================

`default_nettype none

module hx_dsp (
    input  wire        clk,
    input  wire        rst,

    // cart WRITE mailbox (byte-granular, matches 65816 stores)
    input  wire        w_we,
    input  wire [3:0]  w_addr,
    input  wire [7:0]  w_data,

    // cart READ mailbox (indexed readback)
    input  wire [2:0]  r_addr,
    output reg  [7:0]  r_data,

    output wire        ready
);
    reg [7:0]  func;
    reg [31:0] arga, argb;
    reg [31:0] result;
    reg signed [39:0] acc;
    reg        busy;

    reg [16:0] div_rem;
    reg [31:0] div_quo;
    reg [5:0]  div_i;

    // signed 16x16 product (one DSP block)
    wire signed [31:0] prod = $signed(arga[15:0]) * $signed(argb[15:0]);
    wire signed [39:0] prod40 = {{8{prod[31]}}, prod};
    // one restoring-division step
    wire [16:0] rem_sh = {div_rem[15:0], div_quo[31]};
    wire        sub_ok = (rem_sh >= {1'b0, argb[15:0]});

    assign ready = ~busy;

    localparam [2:0] S_IDLE=0,S_MUL=1,S_MAC=2,S_DIV=3,S_DIVDONE=4,S_DONE=5;
    reg [2:0] state;

    always @(posedge clk) begin
        if (rst) begin
            state<=S_IDLE; busy<=1'b0; acc<=40'd0; result<=32'd0;
        end else begin
            // byte-assemble operands (START is w_addr 15, handled below)
            if (w_we) case (w_addr)
                4'd0: func<=w_data;
                4'd1: arga[7:0]  <=w_data; 4'd2: arga[15:8] <=w_data;
                4'd3: arga[23:16]<=w_data; 4'd4: arga[31:24]<=w_data;
                4'd5: argb[7:0]  <=w_data; 4'd6: argb[15:8] <=w_data;
                4'd7: argb[23:16]<=w_data; 4'd8: argb[31:24]<=w_data;
                default: ;
            endcase

            case (state)
            S_IDLE: if (w_we && w_addr==4'd15) begin
                busy<=1'b1;
                case (func[1:0])
                    2'd0: state<=S_MUL;
                    2'd1: state<=S_MAC;
                    2'd2: begin div_rem<=17'd0; div_quo<=arga; div_i<=6'd0; state<=S_DIV; end
                    2'd3: begin acc<=prod40; result<=prod[31:0]; state<=S_DONE; end  // MACINIT
                endcase
            end
            S_MUL: begin result <= prod[31:0]; state<=S_DONE; end
            S_MAC: begin acc <= acc + prod40; result <= (acc + prod40); state<=S_DONE; end
            S_DIV: begin
                if (argb[15:0]==16'd0) begin result<=32'hFFFFFFFF; state<=S_DONE; end
                else begin
                    div_rem <= sub_ok ? (rem_sh - {1'b0,argb[15:0]}) : rem_sh;
                    div_quo <= {div_quo[30:0], sub_ok};
                    div_i   <= div_i + 6'd1;
                    if (div_i==6'd31) state<=S_DIVDONE;
                end
            end
            S_DIVDONE: begin result <= div_quo; state<=S_DONE; end
            S_DONE: begin busy<=1'b0; state<=S_IDLE; end
            default: state<=S_IDLE;
            endcase
        end
    end

    // indexed readback
    always @(*) case (r_addr)
        3'd0: r_data = result[7:0];
        3'd1: r_data = result[15:8];
        3'd2: r_data = result[23:16];
        3'd3: r_data = result[31:24];
        3'd4: r_data = {7'd0, ready};
        default: r_data = 8'd0;
    endcase
endmodule

`default_nettype wire
