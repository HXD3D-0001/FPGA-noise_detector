// Counts are registered Gray in their source domain, changing by at most one
// count per source edge. TEST progress is a same-proc-domain lifetime count.
// Sticky faults count once per arm. Rearm is explicit; disabled preserves fault.
module input_watchdog #(
 parameter integer PROGRESS_SYNCHRONIZED=0, BCK_TIMEOUT_CYCLES=50000,SLOT_TIMEOUT_CYCLES=500000,TEST_TIMEOUT_CYCLES=500000
)(input wire proc_clk,rst,rearm,enable,test_mode,
 input wire[63:0]bck_progress_gray,slot_progress_gray,test_progress,input wire format_fault,
 output reg fault,output reg[3:0]fault_reason,output reg[63:0]fault_count);
 (* ASYNC_REG="TRUE" *)reg[2:0]reset_pipe;
 always@(posedge proc_clk or posedge rst)
  if(rst)reset_pipe<=3'b111;else reset_pipe<={reset_pipe[1:0],1'b0};
 wire local_reset=rst|reset_pipe[2];
 // Set only when both buses already passed one canonical proc synchronizer.
 // Re-synchronizing the same Gray bus twice can yield incoherent observations.
 wire[63:0]bck_sync,slot_sync;
 generate if(PROGRESS_SYNCHRONIZED)begin: proc_progress
  assign bck_sync=bck_progress_gray;assign slot_sync=slot_progress_gray;
 end else begin: async_progress
  (* ASYNC_REG="TRUE" *)reg[63:0]bck_meta,bck_local,slot_meta,slot_local;
  always@(posedge proc_clk or posedge local_reset)
   if(local_reset)begin bck_meta<=0;bck_local<=0;slot_meta<=0;slot_local<=0;end
   else begin bck_meta<=bck_progress_gray;bck_local<=bck_meta;slot_meta<=slot_progress_gray;slot_local<=slot_meta;end
  assign bck_sync=bck_local;assign slot_sync=slot_local;
 end endgenerate
 reg[63:0]last_bck,last_slot,last_test,bck_timer,slot_timer,test_timer;
 always@(posedge proc_clk or posedge local_reset)begin
  if(local_reset)begin
   last_bck<=0;last_slot<=0;last_test<=0;bck_timer<=0;slot_timer<=0;test_timer<=0;
   fault<=0;fault_reason<=0;fault_count<=0;
  end else begin
   last_bck<=bck_sync;last_slot<=slot_sync;last_test<=test_progress;
   if(rearm||!enable)begin
    bck_timer<=0;slot_timer<=0;test_timer<=0;
    if(rearm)begin fault<=0;fault_reason<=0;end
   end else if(!fault)begin
    if(test_mode)begin
     if(test_progress!=last_test)test_timer<=0;
     else if(test_timer+1>=TEST_TIMEOUT_CYCLES)begin fault<=1;fault_reason<=6;fault_count<=fault_count+1'b1;end
     else test_timer<=test_timer+1'b1;
    end else begin
     if(bck_sync!=last_bck)bck_timer<=0;else if(bck_timer<BCK_TIMEOUT_CYCLES)bck_timer<=bck_timer+1'b1;
     if(slot_sync!=last_slot)slot_timer<=0;else if(slot_timer<SLOT_TIMEOUT_CYCLES)slot_timer<=slot_timer+1'b1;
     if(format_fault)begin fault<=1;fault_reason<=7;fault_count<=fault_count+1'b1;end
     else if(bck_sync==last_bck&&bck_timer+1>=BCK_TIMEOUT_CYCLES)begin fault<=1;fault_reason<=4;fault_count<=fault_count+1'b1;end
     else if(slot_sync==last_slot&&slot_timer+1>=SLOT_TIMEOUT_CYCLES)begin fault<=1;fault_reason<=5;fault_count<=fault_count+1'b1;end
    end
   end
  end
 end
 initial if(BCK_TIMEOUT_CYCLES<1||SLOT_TIMEOUT_CYCLES<1||TEST_TIMEOUT_CYCLES<1)$error("invalid watchdog parameters");
endmodule
