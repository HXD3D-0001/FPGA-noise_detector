// One owner for the TEST datapath. A request level is executed once per tag.
// DMA acknowledgement is a PS observation of completion, never a register write
// that merely starts reset. Timed-out ownership remains quarantined.
module control_service #(parameter TIMEOUT_TICKS=4999990,
 parameter[63:0]BOOT_ORIGIN=0)(
 input wire clk,rst,
 input wire command_req,input wire[2:0]command_kind,input wire[31:0]command_seq,
 input wire flow_req,input wire[1:0]flow_mask,input wire[31:0]flow_seq,
 output reg flow_ack,flow_error,output reg[31:0]flow_ack_seq,
 output reg command_ack,command_error,output reg[31:0]command_ack_seq,
 input wire commit_req,input wire[31:0]commit_mode,commit_config_id,commit_seq,
 output reg commit_ack,commit_error,output reg[31:0]commit_ack_seq,
 input wire snapshot_req,input wire[31:0]snapshot_request_seq,
 output reg snapshot_ack,snapshot_error,snapshot_invalidate,output reg[31:0]snapshot_ack_seq,
 output reg[1023:0]snapshot_counters_i,output reg[63:0]snapshot_energy_flush_i,
 output reg[63:0]snapshot_pcm_flush_i,
 output reg[255:0]snapshot_sequences_i,
 output wire[31:0]snapshot_source_id_i,output reg snapshot_position_valid_i,
 output reg[63:0]snapshot_latest_raw_i,snapshot_origin_raw_i,
 input wire recovery_req,output reg recovery_ack,
 output wire[31:0]state_i,status_i,output reg[31:0]irq_events_i,
 output reg[31:0]dma_reset_request_seq_i,output reg[1:0]dma_reset_request_mask_i,
 output reg dma_reset_request_reset_i,
 input wire dma_reset_ack_valid,input wire[31:0]dma_reset_ack_seq,input wire[1:0]dma_reset_ack_mask,
 input wire drained,analysis_ready,mode_ready,mode_rejected,input wire[1:0]mode_active,
 output reg mode_valid,output wire stopped,output reg[1:0]mode_select,
 output reg enable,start,clear_history,clear_records,energy_invalidate,
 output reg spectrum_flush_req,dma_spectrum_reset_done,dma_pcm_abort,
 output reg spectrum_output_hold,pcm_output_hold,
 input wire spectrum_flush_pending,spectrum_flush_done,input wire[31:0]spectrum_flush_request_seq,
 output reg[31:0]dma_spectrum_reset_seq,
 output reg[63:0]capture_id,origin_raw,output reg[31:0]config_id,
 input wire[63:0]raw_count,latest_raw,energy_flush_count,pcm_flush_count,
 input wire source_position_valid,input wire[31:0]energy_occupancy,
 input wire[1023:0]counters_i,input wire[255:0]sequences_i
);
 localparam STOPPED=0,WARMUP=4,RUN=5,DRAIN=6,FAULT=7,RESETWAIT=8,COMMITTING=9;
 localparam OP_NONE=0,OP_COMMIT=1,OP_START=2,OP_STOP=3,OP_RESET=4,OP_RECOVERY=5,OP_FLOW=6;
 reg[3:0]state,operation;
 reg[31:0]ticks,op_seq,seen_command,seen_commit,seen_snapshot,requested_mode,requested_config;
 reg command_seen,commit_seen,snapshot_seen,armed,started,snapshot_pending,reset_applied;
 reg[7:0]reason;reg[2:0]phase;
 reg flow_seen;reg[31:0]seen_flow;reg[1:0]selected_flow;
 assign state_i={28'd0,state};
 // TEST source present; config armed; recovery; draining; reason in[23:16].
 assign status_i={8'd0,reason,11'd0,analysis_ready,(state==DRAIN),(state==FAULT||state==RESETWAIT),armed,1'b1}|
  (operation==OP_FLOW?32'h20:0);
 assign stopped=(state==STOPPED||state==COMMITTING);
 assign snapshot_source_id_i=1; // TEST only; external ADC is not instantiated.
 wire dma_match=dma_reset_ack_valid&&dma_reset_request_mask_i!=0&&
  dma_reset_ack_seq==dma_reset_request_seq_i&&dma_reset_ack_mask==dma_reset_request_mask_i;
 wire command_new=command_req&&(!command_seen||seen_command!=command_seq);
 wire commit_new=commit_req&&(!commit_seen||seen_commit!=commit_seq);
 wire timed=ticks>=TIMEOUT_TICKS-1;
 task launch_reset;
 begin
  enable<=0;armed<=0;started<=0;snapshot_invalidate<=1;
  spectrum_output_hold<=1;pcm_output_hold<=1;
  state<=RESETWAIT;phase<=0;reset_applied<=0;ticks<=0;
  if(dma_reset_request_seq_i==32'hffffffff)begin state<=FAULT;reason<=7;end
  else begin
   dma_reset_request_seq_i<=dma_reset_request_seq_i+1'b1;
   dma_reset_request_mask_i<=3;dma_reset_request_reset_i<=1;spectrum_flush_req<=1;
  end
 end
 endtask
 always @(posedge clk)begin
  if(rst)begin
   state<=STOPPED;operation<=OP_NONE;ticks<=0;phase<=0;reason<=0;op_seq<=0;
   command_ack<=0;command_error<=0;command_ack_seq<=0;commit_ack<=0;commit_error<=0;commit_ack_seq<=0;
   flow_ack<=0;flow_error<=0;flow_ack_seq<=0;flow_seen<=0;seen_flow<=0;selected_flow<=0;
   snapshot_ack<=0;snapshot_error<=0;snapshot_ack_seq<=0;snapshot_invalidate<=0;snapshot_pending<=0;
   snapshot_counters_i<=0;snapshot_energy_flush_i<=0;snapshot_pcm_flush_i<=0;snapshot_sequences_i<=0;
   snapshot_position_valid_i<=0;snapshot_latest_raw_i<=0;snapshot_origin_raw_i<=BOOT_ORIGIN;
   seen_command<=0;seen_commit<=0;seen_snapshot<=0;command_seen<=0;commit_seen<=0;snapshot_seen<=0;
   armed<=0;started<=0;requested_mode<=0;requested_config<=0;config_id<=0;capture_id<=0;origin_raw<=BOOT_ORIGIN;
   mode_valid<=0;mode_select<=0;enable<=0;start<=0;clear_history<=0;clear_records<=0;energy_invalidate<=0;
   spectrum_flush_req<=0;dma_spectrum_reset_done<=0;dma_pcm_abort<=0;dma_spectrum_reset_seq<=0;
   spectrum_output_hold<=0;pcm_output_hold<=0;
   dma_reset_request_seq_i<=0;dma_reset_request_mask_i<=0;dma_reset_request_reset_i<=0;
   recovery_ack<=0;irq_events_i<=0;reset_applied<=0;
  end else begin
   command_ack<=0;command_error<=0;commit_ack<=0;commit_error<=0;snapshot_ack<=0;snapshot_error<=0;
   flow_ack<=0;flow_error<=0;
   snapshot_invalidate<=0;recovery_ack<=0;irq_events_i<=0;mode_valid<=0;start<=0;clear_history<=0;
   clear_records<=0;energy_invalidate<=0;spectrum_flush_req<=0;dma_spectrum_reset_done<=0;dma_pcm_abort<=0;
   if(state==WARMUP&&operation==OP_NONE&&analysis_ready)state<=RUN;
   if(operation!=OP_NONE&&ticks!=32'hffffffff)ticks<=ticks+1'b1;
   if(snapshot_pending)begin snapshot_ack<=1;snapshot_pending<=0;end
   if(snapshot_req&&(!snapshot_seen||seen_snapshot!=snapshot_request_seq))begin
    seen_snapshot<=snapshot_request_seq;snapshot_seen<=1;snapshot_ack_seq<=snapshot_request_seq;
    snapshot_counters_i<=counters_i;snapshot_counters_i[63:0]<=capture_id;
    snapshot_energy_flush_i<=energy_flush_count;snapshot_pcm_flush_i<=pcm_flush_count;snapshot_sequences_i<=sequences_i;
    snapshot_position_valid_i<=started&&source_position_valid;
    snapshot_latest_raw_i<=started&&source_position_valid?latest_raw:origin_raw;
    snapshot_origin_raw_i<=origin_raw;snapshot_pending<=1;
   end
   if(command_new)begin command_seen<=1;seen_command<=command_seq;end
   if(commit_new)begin commit_seen<=1;seen_commit<=commit_seq;end
   if(operation==OP_NONE)begin
    if(recovery_req)begin operation<=OP_RECOVERY;launch_reset();end
    else if(flow_req&&(!flow_seen||seen_flow!=flow_seq))begin
     flow_seen<=1;seen_flow<=flow_seq;flow_ack_seq<=flow_seq;
     if((state!=RUN&&state!=WARMUP)||(flow_mask!=1&&flow_mask!=2)||dma_reset_request_seq_i==32'hffffffff)begin
      flow_ack<=1;flow_error<=1;reason<=2;
     end else begin
      selected_flow<=flow_mask;operation<=OP_FLOW;op_seq<=flow_seq;reset_applied<=0;phase<=0;ticks<=0;
      dma_reset_request_seq_i<=dma_reset_request_seq_i+1'b1;dma_reset_request_mask_i<=flow_mask;
      dma_reset_request_reset_i<=1;spectrum_flush_req<=flow_mask[0];reason<=0;
      spectrum_output_hold<=flow_mask[0];pcm_output_hold<=flow_mask[1];
     end
    end
    else if(command_new)begin
     command_ack_seq<=command_seq;reason<=0;
     if(command_kind==3)begin operation<=OP_RESET;op_seq<=command_seq;launch_reset();end
     else if(command_kind==2&&(state==RUN||state==WARMUP||state==STOPPED))begin
      enable<=0;armed<=0;operation<=OP_STOP;op_seq<=command_seq;state<=DRAIN;phase<=0;ticks<=0;
     end else if(command_kind==1&&state==STOPPED&&armed&&requested_mode==3&&drained)begin
      enable<=1;start<=1;clear_history<=1;started<=1;armed<=0;state<=WARMUP;
      operation<=OP_START;op_seq<=command_seq;ticks<=0;snapshot_invalidate<=1;
     end else begin command_ack<=1;command_error<=1;reason<=command_kind==1&&requested_mode!=3?8'd9:8'd2;end
    end else if(commit_new)begin
     commit_ack_seq<=commit_seq;reason<=0;
     if(state!=STOPPED||!drained||energy_occupancy!=0||!mode_ready||commit_config_id==0||commit_mode>3||
      capture_id==64'hffffffffffffffff||raw_count>64'hffffffffffffffff-BOOT_ORIGIN)begin
      commit_ack<=1;commit_error<=1;reason<=2;
     end else begin
      operation<=OP_COMMIT;op_seq<=commit_seq;requested_mode<=commit_mode;requested_config<=commit_config_id;
      state<=COMMITTING;phase<=0;mode_select<=commit_mode==3?2'd0:commit_mode[1:0];mode_valid<=1;ticks<=0;
     end
    end
   end else if(operation==OP_COMMIT)begin
    if(phase==0)phase<=1;
    else if(mode_rejected||timed||!commit_req)begin
     commit_ack<=commit_req;commit_error<=1;commit_ack_seq<=op_seq;state<=STOPPED;operation<=OP_NONE;armed<=0;reason<=5;
    end else if(mode_active==mode_select)begin
     capture_id<=capture_id+1'b1;origin_raw<=BOOT_ORIGIN+raw_count;config_id<=requested_config;
     armed<=1;started<=0;clear_history<=1;energy_invalidate<=1;snapshot_invalidate<=1;
     commit_ack<=1;commit_ack_seq<=op_seq;state<=STOPPED;operation<=OP_NONE;
    end
   end else if(operation==OP_START)begin
    if(!command_req||timed)begin
     command_ack<=command_req;command_error<=1;command_ack_seq<=op_seq;reason<=6;
     operation<=OP_RECOVERY;launch_reset();
    end else if(source_position_valid&&ticks>1)begin
     state<=analysis_ready?RUN:WARMUP;command_ack<=1;command_ack_seq<=op_seq;operation<=OP_NONE;
    end
   end else if(operation==OP_STOP)begin
    if(!command_req||timed)begin
     command_ack<=command_req;command_error<=1;command_ack_seq<=op_seq;reason<=6;
     operation<=OP_RECOVERY;launch_reset();
    end else if(phase==0&&drained)begin clear_history<=1;phase<=1;end
    else if(phase==1)begin
     if(dma_reset_request_seq_i==32'hffffffff)begin state<=FAULT;reason<=7;end
     else begin dma_reset_request_seq_i<=dma_reset_request_seq_i+1'b1;dma_reset_request_mask_i<=3;dma_reset_request_reset_i<=0;phase<=2;end
    end else if(phase==2&&dma_match)begin
     dma_reset_request_mask_i<=0;state<=STOPPED;started<=0;command_ack<=1;command_ack_seq<=op_seq;operation<=OP_NONE;
    end
   end else if(operation==OP_FLOW)begin
    if(!reset_applied&&dma_match&&(!selected_flow[0]||spectrum_flush_pending))begin
     dma_reset_request_mask_i<=0;dma_spectrum_reset_done<=selected_flow[0];dma_spectrum_reset_seq<=spectrum_flush_request_seq;
     dma_pcm_abort<=selected_flow[1];reset_applied<=1;phase<=0;
    end else if(reset_applied)begin
     if(phase<3)phase<=phase+1'b1;
     else if(!selected_flow[0]||!spectrum_flush_pending)begin
      spectrum_output_hold<=0;pcm_output_hold<=0;
      flow_ack<=1;flow_ack_seq<=op_seq;operation<=OP_NONE;reason<=0;
     end
    end
    if(timed&&!reset_applied)begin reason<=6;irq_events_i<=4;end
    // The unselected stream, source/grid and energy owner continue unchanged.
   end else if(operation==OP_RESET||operation==OP_RECOVERY)begin
    if(!reset_applied&&dma_match&&spectrum_flush_pending)begin
     dma_reset_request_mask_i<=0;dma_spectrum_reset_done<=1;dma_spectrum_reset_seq<=spectrum_flush_request_seq;
     dma_pcm_abort<=1;clear_records<=1;energy_invalidate<=1;clear_history<=1;snapshot_invalidate<=1;
     reset_applied<=1;phase<=0;
    end else if(reset_applied)begin
     if(phase<3)phase<=phase+1'b1;
     else if(drained)begin
      spectrum_output_hold<=0;pcm_output_hold<=0;
      state<=STOPPED;reason<=0;recovery_ack<=1;
      if(operation==OP_RESET&&command_req)begin command_ack<=1;command_ack_seq<=op_seq;end
      operation<=OP_NONE;
     end
    end
    if(timed&&!reset_applied)begin
     state<=FAULT;reason<=6;irq_events_i<=4;
     if(operation==OP_RESET&&command_req)begin command_ack<=1;command_error<=1;command_ack_seq<=op_seq;operation<=OP_RECOVERY;end
     // Keep the same outstanding reset tag/mask. A late genuine ACK can recover.
    end
   end
  end
 end
 initial if(TIMEOUT_TICKS<8)$error("service timeout must allow reset and drain synchronizers");
endmodule
