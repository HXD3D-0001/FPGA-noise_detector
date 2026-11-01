// Proc-domain command owner for the audio clock boundary. Not an AXI/PS ABI.
// Requests are levels held through matching ACK. Tags never wrap in one boot.
// Restart/clear only while the whole capture/data-owner STOP barrier is true.
module audio_clock_service #(parameter integer TIMEOUT_CYCLES=250000000)(
 input wire clk,rst,allow_commands,mclk_ready,clock_fault,restart_pending,
 input wire command_req,input wire[1:0]command_kind,input wire[31:0]command_seq,
 output reg command_ack,command_error,output reg[31:0]command_ack_seq,
 output reg[3:0]command_reason,output wire busy,
 output reg restart_clocks,clear_clock_fault,
 input wire snapshot_req,input wire[31:0]snapshot_request_seq,
 input wire[63:0]capture_id,adc_count,test_count,latest_raw,capture_fault_count,
 input wire[31:0]config_id,input wire[3:0]capture_state,capture_reason,
 input wire active_test,running,start_ready,stop_complete,position_valid,
 output reg snapshot_ack,snapshot_error,snapshot_valid,
 output reg[31:0]snapshot_ack_seq,output reg[511:0]snapshot_data,
 output reg[63:0]snapshot_age
);
 reg command_seen,snapshot_seen;
 reg[31:0]seen_command,max_command,op_seq,seen_snapshot,max_snapshot;
 reg[1:0]operation;reg[63:0]ticks;reg op_busy;
 // Timeout reports failure but cannot cancel a request in a stopped domain.
 assign busy=op_busy||restart_pending;
 // Key changes invalidate stale observations without rewriting the held data.
 wire[107:0]key={capture_id,config_id,capture_state,capture_reason,
   active_test,mclk_ready,clock_fault,position_valid};
 reg[107:0]snapshot_key;
 wire new_command=command_req&&(!command_seen||command_seq!=seen_command);
 wire new_snapshot=snapshot_req&&(!snapshot_seen||snapshot_request_seq!=seen_snapshot);
 // Completion requires ticks>=2 and mclk_ready. mclk_ready_proc is the proc
 // domain release chain, NOT the MMCM lock, and it never falls during a
 // restart - so observing a ready fall (seen_unready) is unsatisfiable for
 // every restart, warm or cold. The hardware truth of a completed restart is
 // the CDC round trip: the reference domain applied the toggle (restart_
 // pending rose) and it has settled back to 0. Require exactly that round
 // trip, then the ready release chain. This replaces both the previous
 // seen_unready rule and the ever_ready cold-start exception.
 wire saw_apply=op_busy&&restart_pending;
 reg applied_seen;
 always @(posedge clk or posedge rst)
  if(rst) applied_seen<=0;
  else if(!op_busy) applied_seen<=0;
  else if(saw_apply) applied_seen<=1;
 wire completion_ok=ticks>=2 && mclk_ready && !restart_pending &&
   ((operation==1&&applied_seen)||(operation==2&&!clock_fault));
 task consume_command;
 begin
  command_seen<=1;seen_command<=command_seq;
  if(!command_seen||command_seq>max_command)max_command<=command_seq;
 end endtask
 always @(posedge clk or posedge rst)begin
  if(rst)begin
   command_ack<=0;command_error<=0;command_ack_seq<=0;command_reason<=0;op_busy<=0;
   restart_clocks<=0;clear_clock_fault<=0;command_seen<=0;seen_command<=0;max_command<=0;
   operation<=0;op_seq<=0;ticks<=0;
   snapshot_ack<=0;snapshot_error<=0;snapshot_valid<=0;snapshot_ack_seq<=0;
   snapshot_data<=0;snapshot_age<=64'hffffffffffffffff;snapshot_key<=0;
   snapshot_seen<=0;seen_snapshot<=0;max_snapshot<=0;
  end else begin
   command_ack<=0;command_error<=0;restart_clocks<=0;clear_clock_fault<=0;
   snapshot_ack<=0;snapshot_error<=0;
   if(snapshot_valid&&snapshot_age!=64'hffffffffffffffff)snapshot_age<=snapshot_age+1'b1;
   if(snapshot_key!=key)snapshot_valid<=0;
   if(new_snapshot)begin
    snapshot_seen<=1;seen_snapshot<=snapshot_request_seq;
    snapshot_ack<=1;snapshot_ack_seq<=snapshot_request_seq;
    if(snapshot_seen&&snapshot_request_seq<=max_snapshot)snapshot_error<=1;
    else begin
     max_snapshot<=snapshot_request_seq;snapshot_valid<=1;snapshot_key<=key;snapshot_age<=0;
     snapshot_data<={64'd0,32'd0,config_id,capture_fault_count,latest_raw,test_count,adc_count,capture_id,
       32'h00010000,16'd0,capture_reason,capture_state,busy,position_valid,stop_complete,
       start_ready,running,active_test,clock_fault,mclk_ready};
    end
   end
   if(op_busy)begin
    ticks<=ticks+1'b1;
    // Completion gets the only ACK slot. A concurrent new level request is
    // left unconsumed for the next cycle; the requester must hold its tag.
    if(!allow_commands||ticks+1>=TIMEOUT_CYCLES||completion_ok)begin
     op_busy<=0;command_ack<=1;command_ack_seq<=op_seq;operation<=0;
     if(!allow_commands)begin command_error<=1;command_reason<=2;end
     else if(completion_ok)command_reason<=0;
     else begin command_error<=1;command_reason<=3;end
    end else if(new_command)begin
     consume_command();command_ack<=1;command_error<=1;command_ack_seq<=command_seq;command_reason<=4;
    end
   end else if(new_command)begin
    consume_command();command_ack_seq<=command_seq;
    if(command_seen&&command_seq<=max_command)begin command_ack<=1;command_error<=1;command_reason<=5;end
    else if(restart_pending)begin command_ack<=1;command_error<=1;command_reason<=4;end
    else if(!allow_commands)begin command_ack<=1;command_error<=1;command_reason<=2;end
    else if(command_kind!=1&&command_kind!=2)begin command_ack<=1;command_error<=1;command_reason<=1;end
    else if(command_kind==2&&!mclk_ready)begin command_ack<=1;command_error<=1;command_reason<=6;end
    else begin
     op_seq<=command_seq;operation<=command_kind;op_busy<=1;ticks<=0;command_reason<=0;
     restart_clocks<=command_kind==1;clear_clock_fault<=command_kind==2;
    end
   end
  end
 end
 initial if(TIMEOUT_CYCLES<4)$error("audio clock command timeout must be >=4");
endmodule
