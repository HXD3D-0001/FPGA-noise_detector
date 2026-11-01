// The source clock remains 50 MHz. CE comes only from proc-clock state, never
// from an asynchronous board signal. BUFGCE passes complete native clock pulses.
// The low-phase sampled handshake mask prevents processing a stale IP ready or
// valid while its clock is stopped. Full reset forces clocks through the gate.
module fft_clock_gate #(parameter ENABLE=0)(
 input wire clk,rst,enable,output wire core_clk,output wire core_active
);
 generate if(ENABLE==0)begin:ungated
  assign core_clk=clk;assign core_active=1'b1;
 end else begin:gated
  wire requested=rst||enable;
  reg sampled;
  always @(negedge clk)sampled<=requested;
  BUFGCE #(.SIM_DEVICE("7SERIES")) native_clock(.I(clk),.CE(requested),.O(core_clk));
  assign core_active=sampled;
 end endgenerate
endmodule
