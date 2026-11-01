// One outstanding bundled snapshot. A timed-out transaction remains quarantined
// until its ACK returns: a stopped reference clock cannot alias a later request.
// status_in must be synchronous to ref_clk; the held bus is not bit-synchronized.
module clock_status_bridge #(parameter integer STATUS_WIDTH=256,TIMEOUT_CYCLES=5000000)(
 input wire proc_clk,ref_clk,rst,request,input wire[STATUS_WIDTH-1:0]status_in,
 output reg[STATUS_WIDTH-1:0]status_out,output reg valid,busy,timeout,rejected,
 output reg[63:0]snapshot_seq64,age64);
 (* ASYNC_REG="TRUE" *)reg[2:0]proc_reset_pipe,ref_reset_pipe;
 always@(posedge proc_clk or posedge rst)
  if(rst)proc_reset_pipe<=3'b111;else proc_reset_pipe<={proc_reset_pipe[1:0],1'b0};
 always@(posedge ref_clk or posedge rst)
  if(rst)ref_reset_pipe<=3'b111;else ref_reset_pipe<={ref_reset_pipe[1:0],1'b0};
 wire pr=rst|proc_reset_pipe[2],rr=rst|ref_reset_pipe[2];
 reg request_toggle,ack_toggle,outstanding,have_snapshot;
 (* ASYNC_REG="TRUE" *)reg[1:0]request_sync;
 (* ASYNC_REG="TRUE" *)reg[2:0]ack_sync;
 reg[STATUS_WIDTH-1:0]held_status;
 reg[63:0]timer;
 always@(posedge ref_clk or posedge rr)begin
  if(rr)begin request_sync<=0;ack_toggle<=0;held_status<=0;end
  else begin
   request_sync<={request_sync[0],request_toggle};
   if(request_sync[1]!=ack_toggle)begin held_status<=status_in;ack_toggle<=request_sync[1];end
  end
 end
 always@(posedge proc_clk or posedge pr)begin
  if(pr)begin request_toggle<=0;ack_sync<=0;outstanding<=0;have_snapshot<=0;
   status_out<=0;valid<=0;busy<=0;timeout<=0;rejected<=0;snapshot_seq64<=0;age64<=64'hffffffffffffffff;timer<=0;
  end else begin
   ack_sync<={ack_sync[1:0],ack_toggle};rejected<=0;
   if(have_snapshot&&age64!=64'hffffffffffffffff)age64<=age64+1'b1;
   if(request)begin
    if(outstanding)rejected<=1;
    else begin request_toggle<=~request_toggle;outstanding<=1;busy<=1;valid<=0;timeout<=0;timer<=0;end
   end
   if(outstanding)begin
    if(ack_sync[2]==request_toggle)begin
     outstanding<=0;
     if(busy)begin status_out<=held_status;valid<=1;busy<=0;timeout<=0;
      snapshot_seq64<=snapshot_seq64+1'b1;age64<=0;have_snapshot<=1;end
    end else if(busy)begin
     if(timer+1>=TIMEOUT_CYCLES)begin busy<=0;valid<=0;timeout<=1;end
     else timer<=timer+1'b1;
    end
   end
  end
 end
 initial if(STATUS_WIDTH<1||TIMEOUT_CYCLES<1)$error("invalid snapshot parameters");
endmodule
