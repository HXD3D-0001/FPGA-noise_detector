// Lock qualification only. This does NOT claim BCLK is running or ADC is settled.
// Loss asserts resets immediately; release takes three destination clock edges.
module audio_clock_state(
 input wire proc_clk,ref_clk,bclk,mclk,rst,locked_raw,clear_fault,
 output wire mclk_ready_proc,mclk_ready_ref,bclk_reset,mclk_reset,
 output reg clock_fault_latched
);
 wire unavailable = rst | !locked_raw;
 (* ASYNC_REG="TRUE" *) reg [2:0] proc_release,ref_release,bclk_release,mclk_release;
 always @(posedge proc_clk or posedge unavailable)
  if(unavailable) proc_release<=3'b111;
  else proc_release<={proc_release[1:0],1'b0};
 always @(posedge ref_clk or posedge unavailable)
  if(unavailable) ref_release<=3'b111;
  else ref_release<={ref_release[1:0],1'b0};
 always @(posedge bclk or posedge unavailable)
  if(unavailable) bclk_release<=3'b111;
  else bclk_release<={bclk_release[1:0],1'b0};
 always @(posedge mclk or posedge unavailable)
  if(unavailable) mclk_release<=3'b111;
  else mclk_release<={mclk_release[1:0],1'b0};
 assign mclk_reset=mclk_release[2];
 assign mclk_ready_proc=!proc_release[2];
 assign mclk_ready_ref=!ref_release[2];
 assign bclk_reset=bclk_release[2];
 reg was_ready;
 always @(posedge proc_clk or posedge rst) begin
  if(rst) begin was_ready<=0;clock_fault_latched<=0;end
  else begin
   was_ready<=mclk_ready_proc;
   if(clear_fault && mclk_ready_proc) clock_fault_latched<=0;
   if(was_ready && !mclk_ready_proc) clock_fault_latched<=1;
  end
 end
endmodule
