// Tagged proc-domain adapter for capture_processing_top / audio_capture_top.
// Distinct from the legacy TEST-only LFN1 owner. Never connect both owners.
// stop_complete is the full datapath/energy/DMA barrier, not merely !running.
module capture_control_service #(
 parameter integer TIMEOUT_CYCLES=250000000,
 parameter[63:0]BOOT_ORIGIN=0
)(
 input wire clk,rst,
 input wire commit_req,input wire[31:0]commit_seq,commit_mode,commit_source,
 input wire[31:0]commit_channel,commit_pattern,commit_config_id,
 output reg commit_ack,commit_error,output reg[31:0]commit_ack_seq,
 output reg[3:0]commit_reason,output reg commit_timed_out,
 input wire command_req,input wire[1:0]command_kind,input wire[31:0]command_seq,
 output reg command_ack,command_error,output reg[31:0]command_ack_seq,
 output reg[3:0]command_reason,output reg command_timed_out,
 input wire stop_complete,start_ready,running,command_rejected,clock_control_busy,
 input wire[3:0]frontend_state,fault_reason,
 input wire[63:0]active_capture_id,input wire[31:0]active_config_id,
 input wire active_test_mode,mode_ready,mode_rejected,input wire[1:0]mode_active,
 output reg mode_valid,output reg[1:0]mode_select,
 output reg start_req,stop_req,requested_test_mode,channel_select,
 output reg[1:0]pattern_id,output reg[31:0]requested_config_id,
 output reg[63:0]next_capture_id,output wire[63:0]origin_raw,
 output reg armed,output wire busy,output reg quarantined,
 output wire[2:0]operation,output reg[3:0]last_fault_reason
);
 localparam IDLE=0,COMMIT=1,START=2,STOP=3,RECOVER=4;
 localparam BAD_REQUEST=1,BAD_STATE=2,BAD_CONFIG=3,STALE_TAG=4,
            EXHAUSTED=5,TIMEOUT=6,FRONTEND=7,MODE_ERROR=8;
 reg[2:0]op;reg[31:0]ticks,op_tag;
 reg command_seen,commit_seen;reg[31:0]seen_command,seen_commit,max_command,max_commit;
 reg[63:0]reserved_id;reg[1:0]pending_mode,pending_pattern,committed_mode;
 reg pending_test,pending_channel;reg[31:0]pending_config;
 reg start_failed;reg[3:0]start_failure;
 wire new_command=command_req&&(!command_seen||seen_command!=command_seq);
 wire new_commit=commit_req&&(!commit_seen||seen_commit!=commit_seq);
 wire[63:0]high_id=reserved_id>active_capture_id?reserved_id:active_capture_id;
 wire expired=ticks>=TIMEOUT_CYCLES-1;
 assign origin_raw=BOOT_ORIGIN; // lifetime source seed, never shifted per capture
 assign busy=op!=IDLE;assign operation=op;
 always@(posedge clk or posedge rst)begin
  if(rst)begin
   op<=IDLE;ticks<=0;op_tag<=0;command_seen<=0;commit_seen<=0;
   seen_command<=0;seen_commit<=0;max_command<=0;max_commit<=0;
   command_ack<=0;command_error<=0;command_ack_seq<=0;command_reason<=0;command_timed_out<=0;
   commit_ack<=0;commit_error<=0;commit_ack_seq<=0;commit_reason<=0;commit_timed_out<=0;
   reserved_id<=0;next_capture_id<=0;pending_mode<=0;pending_pattern<=0;committed_mode<=0;
   pending_test<=0;pending_channel<=0;pending_config<=0;
   requested_test_mode<=0;channel_select<=0;pattern_id<=0;requested_config_id<=0;
   armed<=0;quarantined<=0;mode_valid<=0;mode_select<=0;start_req<=0;stop_req<=0;
   start_failed<=0;start_failure<=0;last_fault_reason<=0;
  end else begin
   command_ack<=0;commit_ack<=0;mode_valid<=0;start_req<=0;stop_req<=0;
   // The datapath's start_ready validates next_capture_id. Prepare the next
   // candidate before asking readiness; reserve it only when START is sent.
   if(op==IDLE&&high_id!=64'hffffffffffffffff)next_capture_id<=high_id+1'b1;
   if(op!=IDLE&&ticks<TIMEOUT_CYCLES)ticks<=ticks+1'b1;
   case(op)
    IDLE:begin
     // Source faults close the segment even with no PS polling. No data owner
     // is reset or acknowledged here; published records must still drain.
     if(frontend_state==7)begin
      op<=RECOVER;ticks<=0;armed<=0;quarantined<=1;stop_req<=1;last_fault_reason<=fault_reason;
     end else if(new_command)begin
      command_seen<=1;seen_command<=command_seq;command_ack_seq<=command_seq;
      command_error<=0;command_reason<=0;command_timed_out<=0;
      if(command_seq==0||(command_seen&&command_seq<=max_command))begin
       command_ack<=1;command_error<=1;command_reason<=STALE_TAG;
      end else begin
       max_command<=command_seq;
       if(command_kind!=1&&command_kind!=2)begin command_ack<=1;command_error<=1;command_reason<=BAD_REQUEST;end
       else if(command_kind==2)begin
        armed<=0;stop_req<=1;op<=STOP;op_tag<=command_seq;ticks<=0;
       end else if(!armed||!start_ready||!stop_complete||clock_control_busy)begin
        command_ack<=1;command_error<=1;command_reason<=BAD_STATE;
       end else if(high_id==64'hffffffffffffffff)begin
        command_ack<=1;command_error<=1;command_reason<=EXHAUSTED;armed<=0;
       end else begin
        next_capture_id<=high_id+1'b1;reserved_id<=high_id+1'b1;
        start_req<=1;armed<=0;op<=START;op_tag<=command_seq;ticks<=0;
        start_failed<=0;start_failure<=0;
       end
      end
     end else if(new_commit)begin
      commit_seen<=1;seen_commit<=commit_seq;commit_ack_seq<=commit_seq;
      commit_error<=0;commit_reason<=0;commit_timed_out<=0;
      if(commit_seq==0||(commit_seen&&commit_seq<=max_commit))begin
       commit_ack<=1;commit_error<=1;commit_reason<=STALE_TAG;
      end else begin
       max_commit<=commit_seq;
       if(!stop_complete||!mode_ready||clock_control_busy)begin commit_ack<=1;commit_error<=1;commit_reason<=BAD_STATE;end
       else if(commit_mode>2||commit_source>1||commit_channel>1||commit_pattern>3||commit_config_id==0||
        (requested_config_id==commit_config_id&&requested_config_id!=0&&
         (committed_mode!=commit_mode[1:0]||requested_test_mode!=commit_source[0]||channel_select!=commit_channel[0]||pattern_id!=commit_pattern[1:0])))begin
        commit_ack<=1;commit_error<=1;commit_reason<=BAD_CONFIG;
       end else begin
        pending_mode<=commit_mode[1:0];pending_test<=commit_source[0];pending_channel<=commit_channel[0];
        pending_pattern<=commit_pattern[1:0];pending_config<=commit_config_id;
        mode_select<=commit_mode[1:0];mode_valid<=1;armed<=0;op<=COMMIT;op_tag<=commit_seq;ticks<=0;
       end
      end
     end
    end
    COMMIT:begin
     if(mode_rejected||!stop_complete||clock_control_busy)begin
      commit_ack<=1;commit_error<=1;commit_ack_seq<=op_tag;commit_reason<=MODE_ERROR;op<=IDLE;
     end else if(ticks>=2&&mode_ready&&mode_active==pending_mode)begin
      requested_test_mode<=pending_test;channel_select<=pending_channel;pattern_id<=pending_pattern;
      requested_config_id<=pending_config;committed_mode<=pending_mode;armed<=1;commit_ack<=1;commit_error<=0;commit_ack_seq<=op_tag;op<=IDLE;
     end else if(expired)begin
      commit_ack<=1;commit_error<=1;commit_ack_seq<=op_tag;commit_reason<=TIMEOUT;commit_timed_out<=1;op<=IDLE;
     end
    end
    START:begin
     if(command_rejected||frontend_state==7||expired)begin
      start_failed<=1;start_failure<=expired?TIMEOUT:FRONTEND;
      command_timed_out<=expired;last_fault_reason<=fault_reason;
      quarantined<=1;stop_req<=1;op<=STOP;ticks<=0;
     end else if(ticks>=2&&running&&active_capture_id==next_capture_id&&
       active_config_id==requested_config_id&&active_test_mode==requested_test_mode)begin
      command_ack<=1;command_error<=0;command_ack_seq<=op_tag;op<=IDLE;
     end
    end
    STOP:begin
     // A drain timeout may leave the frontend in FAULT. Ask it to stop again;
     // the full stop_complete barrier still decides when ownership is safe.
     if(frontend_state==7)stop_req<=1;
     if(expired)begin quarantined<=1;command_timed_out<=1;end
     if(ticks>=2&&stop_complete)begin
      command_ack<=1;command_ack_seq<=op_tag;command_error<=start_failed||command_timed_out;
      command_reason<=start_failed?start_failure:(command_timed_out?TIMEOUT:0);
      quarantined<=0;op<=IDLE;start_failed<=0;
     end
    end
    RECOVER:begin
     if(frontend_state==7)stop_req<=1;
     if(ticks>=2&&stop_complete)begin quarantined<=0;op<=IDLE;end
    end
    default:begin op<=RECOVER;armed<=0;quarantined<=1;stop_req<=1;ticks<=0;end
   endcase
  end
 end
 initial if(TIMEOUT_CYCLES<8)$error("capture control timeout must be >=8");
endmodule
