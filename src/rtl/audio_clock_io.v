// Independent AX7021 audio clock/IO boundary; not yet wired into PS/BD.
// Fixed, documented profile: ref=50 MHz, PCM1808 master at 256fs (48 kHz).
// AMD UG472: CLKOUT0 fractional divide has 1/8 resolution.
module audio_clock_io(
 input wire ref_clk_in,proc_clk,rst,restart_clocks,clear_clock_fault,
 input wire bclk_in,lrck_in,sdata_in,
 output wire mclk_out,ref_clk,bclk,lrck,sdata,mclk,
 output wire stage1_locked_raw,stage2_locked_raw,restart_pending_proc,
 output wire mclk_ready_proc,mclk_ready_ref,bclk_reset,clock_fault_latched
);
 wire ref_ibuf,bclk_ibuf;
 IBUF ref_input(.I(ref_clk_in),.O(ref_ibuf));
 BUFG ref_buffer(.I(ref_ibuf),.O(ref_clk));
 IBUF bclk_input(.I(bclk_in),.O(bclk_ibuf));
 BUFG bclk_buffer(.I(bclk_ibuf),.O(bclk));
 IBUF lrck_input(.I(lrck_in),.O(lrck));
 IBUF data_input(.I(sdata_in),.O(sdata));
 wire fb1,fb1_buf,mid_raw,mid_clk,fb2,fb2_buf,mclk_raw;
 // One outstanding restart. A stopped reference cannot lose the request:
 // hold a proc toggle until the reference domain has applied its local reset.
 reg restart_toggle,applied_toggle;
 (* ASYNC_REG="TRUE" *) reg[1:0] restart_sync,applied_sync;
 (* ASYNC_REG="TRUE" *) reg[2:0] restart_release;
 always @(posedge proc_clk or posedge rst)
  if(rst)begin restart_toggle<=0;applied_sync<=0;end
  else begin
   applied_sync<={applied_sync[0],applied_toggle};
   if(restart_clocks&&!restart_pending_proc)restart_toggle<=~restart_toggle;
  end
 assign restart_pending_proc=restart_toggle!=applied_sync[1];
 always @(posedge ref_clk or posedge rst)
  if(rst)begin restart_sync<=0;applied_toggle<=0;restart_release<=3'b111;end
  else begin
   restart_sync<={restart_sync[0],restart_toggle};
   if(restart_sync[1]!=applied_toggle)begin
    applied_toggle<=restart_sync[1];restart_release<=3'b111;
   end else restart_release<={restart_release[1:0],1'b0};
  end
 wire mmcm_reset=restart_release[2];
 // 50 * 18 / 46.875 = 19.2 MHz; stage-1 VCO = 900 MHz.
 MMCME2_BASE #(.CLKIN1_PERIOD(20.0),.DIVCLK_DIVIDE(1),
  .CLKFBOUT_MULT_F(18.0),.CLKOUT0_DIVIDE_F(46.875),.STARTUP_WAIT("FALSE")) mmcm1(
  .CLKIN1(ref_clk),.CLKFBIN(fb1_buf),.CLKFBOUT(fb1),.CLKOUT0(mid_raw),
  .LOCKED(stage1_locked_raw),.RST(mmcm_reset),.PWRDWN(1'b0));
 BUFG feedback1(.I(fb1),.O(fb1_buf));
 BUFG intermediate_buffer(.I(mid_raw),.O(mid_clk));
 // Release second MMCM only in its own input domain after first-stage lock.
 wire stage2_hold=!stage1_locked_raw;
 (* ASYNC_REG="TRUE" *) reg [2:0] stage2_release;
 always @(posedge mid_clk or posedge stage2_hold)
  if(stage2_hold) stage2_release<=3'b111;
  else stage2_release<={stage2_release[1:0],1'b0};
 // 19.2 * 32 / 50 = 12.288 MHz; stage-2 VCO = 614.4 MHz.
 MMCME2_BASE #(.CLKIN1_PERIOD(52.083333333),.DIVCLK_DIVIDE(1),
  .CLKFBOUT_MULT_F(32.0),.CLKOUT0_DIVIDE_F(50.0),.STARTUP_WAIT("FALSE")) mmcm2(
  .CLKIN1(mid_clk),.CLKFBIN(fb2_buf),.CLKFBOUT(fb2),.CLKOUT0(mclk_raw),
  .LOCKED(stage2_locked_raw),.RST(stage2_release[2]),.PWRDWN(1'b0));
 BUFG feedback2(.I(fb2),.O(fb2_buf));
 BUFG mclk_buffer(.I(mclk_raw),.O(mclk));
 // MMCM RST itself drops LOCKED. Do not inject a ref-domain register
 // into other domains' reset trees: their own release chains use raw lock loss.
 wire locked_raw=stage1_locked_raw & stage2_locked_raw;
 wire mclk_reset;
 audio_clock_state state(.proc_clk(proc_clk),.ref_clk(ref_clk),.bclk(bclk),.mclk(mclk),
  .rst(rst),.locked_raw(locked_raw),.clear_fault(clear_clock_fault),
  .mclk_ready_proc(mclk_ready_proc),.mclk_ready_ref(mclk_ready_ref),
  .bclk_reset(bclk_reset),.mclk_reset(mclk_reset),.clock_fault_latched(clock_fault_latched));
 wire mclk_pad;
 // Dedicated clock forwarding, no fabric register/combinatorial clock gating.
 // Reset may truncate a pulse: the output is explicitly invalid while unlocked.
 ODDR #(.DDR_CLK_EDGE("SAME_EDGE"),.INIT(1'b0),.SRTYPE("ASYNC")) mclk_oddr(
  .C(mclk),.CE(1'b1),.D1(1'b1),.D2(1'b0),.R(mclk_reset),.S(1'b0),.Q(mclk_pad));
 OBUF mclk_output(.I(mclk_pad),.O(mclk_out));
endmodule
