module fft_scheduler #(
 parameter integer INGEST_CYCLES=50000, JOB_CYCLES=5000000, RESET_CYCLES=2
)(input wire clk,rst,abort,frame_abort,
 input wire s_valid,output wire s_ready,input wire signed[23:0]s_data,
 input wire[63:0]s_capture_id,s_first_raw,s_frame_seq,input wire[31:0]s_config_id,
 input wire[7:0]s_flags,input wire[11:0]s_index,input wire s_last,
 output wire[7:0]fft_s_axis_config_tdata,output wire fft_s_axis_config_tvalid,input wire fft_s_axis_config_tready,
 output wire[47:0]fft_s_axis_data_tdata,output wire fft_s_axis_data_tvalid,input wire fft_s_axis_data_tready,output wire fft_s_axis_data_tlast,
 input wire[47:0]fft_m_axis_data_tdata,input wire[23:0]fft_m_axis_data_tuser,input wire fft_m_axis_data_tvalid,output wire fft_m_axis_data_tready,input wire fft_m_axis_data_tlast,
 input wire[7:0]fft_m_axis_status_tdata,input wire fft_m_axis_status_tvalid,output wire fft_m_axis_status_tready,
 output wire fft_aresetn,input wire event_tlast_missing,event_tlast_unexpected,event_data_in_channel_halt,event_data_out_channel_halt,event_status_channel_halt,
 output wire reserve_valid,input wire reserve_ready,output wire[63:0]reserve_capture_id,reserve_first_raw,reserve_frame_seq,
 output wire[31:0]reserve_config_id,output wire[7:0]reserve_flags,
 output reg store_commit,store_abort,
 output wire result_valid,input wire result_ready,output wire signed[23:0]result_real,result_imag,
 output wire[11:0]result_index,output wire[7:0]result_blk_exp,output wire result_last,
 input wire collect_done,collect_error,input wire[7:0]collect_blk_exp,
 output reg[63:0]completed_count,drop_count,timeout_count,protocol_error_count,input_halt_count,output_halt_count,status_halt_count,
 output reg[31:0]last_ingest_cycles,last_job_cycles);

 localparam[2:0] IDLE=0,RESERVE=1,CONFIGURE=2,LOAD=3,WAIT_RESULT=4,RESET_FFT=5,DRAIN=6;
 localparam integer RESET_HOLD=RESET_CYCLES<2?2:RESET_CYCLES;
 reg[2:0]state;
 reg[63:0]capture_id,first_raw,frame_seq;
 reg[31:0]config_id,ingest_timer,job_timer;
 reg[7:0]source_flags,bound_flags,status_exponent;
 reg[11:0]input_index;
 reg reserved,status_seen,sticky_gap,need_drain;
 integer reset_timer;
 wire active=state==RESERVE||state==CONFIGURE||state==LOAD||state==WAIT_RESULT;
 wire loading=state==RESERVE||state==CONFIGURE||state==LOAD;
 wire same_identity=s_capture_id==capture_id&&s_frame_seq==frame_seq;
 wire first_bad=s_capture_id==0||s_config_id==0||!s_flags[0]||s_flags[1]||(|s_flags[7:6])||s_index!=0||s_last;
 wire input_bad=!same_identity||s_first_raw!=first_raw||s_config_id!=config_id||s_flags!=source_flags||s_index!=input_index||s_last!=(input_index==4095);
 wire status_word=fft_m_axis_status_tvalid; // ready is permanently asserted
 wire status_available=status_seen||status_word;
 wire[7:0]candidate_exp=status_seen?status_exponent:fft_m_axis_status_tdata;
 wire deadline=active&&((job_timer>=JOB_CYCLES-1)||(loading&&ingest_timer>=INGEST_CYCLES-1));
 wire output_bad=fft_m_axis_data_tvalid&&(state!=WAIT_RESULT||collect_done||(|fft_m_axis_data_tuser[15:12])||fft_m_axis_data_tuser[23:16]>12);
 wire status_bad=(status_word&&(state!=WAIT_RESULT||status_seen||fft_m_axis_status_tdata>12))||
  (state==WAIT_RESULT&&collect_done&&status_available&&candidate_exp!=collect_blk_exp);
 wire protocol_fault=active&&((state==LOAD&&s_valid&&input_bad)||output_bad||status_bad||collect_error||
  (collect_done&&state!=WAIT_RESULT)||event_tlast_missing||event_tlast_unexpected);
 wire cancelled=active&&(abort||(loading&&frame_abort));
 wire fail_now=deadline||protocol_fault||cancelled;
 assign reserve_valid=state==RESERVE&&!fail_now&&!rst;
 assign reserve_capture_id=capture_id;assign reserve_first_raw=first_raw;assign reserve_frame_seq=frame_seq;
 assign reserve_config_id=config_id;assign reserve_flags=bound_flags;
 assign fft_s_axis_config_tdata=8'h01;
 assign fft_s_axis_config_tvalid=state==CONFIGURE&&!fail_now&&!rst;
 assign fft_s_axis_data_tdata={24'd0,s_data};
 assign fft_s_axis_data_tlast=s_last;
 assign fft_s_axis_data_tvalid=state==LOAD&&s_valid&&!fail_now&&!rst;
 assign s_ready=!rst&&((state==LOAD&&!fail_now&&fft_s_axis_data_tready)||
                      (state==DRAIN&&same_identity&&!frame_abort&&!abort));
 assign result_valid=state==WAIT_RESULT&&fft_m_axis_data_tvalid&&!fail_now&&!rst;
 assign result_real=fft_m_axis_data_tdata[23:0];assign result_imag=fft_m_axis_data_tdata[47:24];
 assign result_index=fft_m_axis_data_tuser[11:0];assign result_blk_exp=fft_m_axis_data_tuser[23:16];
 assign result_last=fft_m_axis_data_tlast;
 assign fft_m_axis_data_tready=state==WAIT_RESULT?(result_ready&&!fail_now&&!rst):1'b1;
 assign fft_m_axis_status_tready=1'b1;
 // Cancellation of an offered native AXIS beat is accompanied by core reset
 // on that same sampling edge, not one edge after valid was withdrawn.
 assign fft_aresetn=!rst&&state!=RESET_FFT&&!fail_now;
`ifndef SYNTHESIS
 initial begin
  if(INGEST_CYCLES<2||JOB_CYCLES<2||RESET_CYCLES<2)$error("invalid FFT deadline/reset parameters");
 end
`endif
 always@(posedge clk)begin
  if(rst)begin
   state<=IDLE;capture_id<=0;first_raw<=0;frame_seq<=0;config_id<=0;source_flags<=0;bound_flags<=0;
   ingest_timer<=0;job_timer<=0;input_index<=0;reserved<=0;status_seen<=0;status_exponent<=0;
   sticky_gap<=0;need_drain<=0;reset_timer<=0;store_commit<=0;store_abort<=0;
   completed_count<=0;drop_count<=0;timeout_count<=0;protocol_error_count<=0;
   input_halt_count<=0;output_halt_count<=0;status_halt_count<=0;last_ingest_cycles<=0;last_job_cycles<=0;
  end else begin
   store_commit<=0;store_abort<=0;
   if(event_data_in_channel_halt)input_halt_count<=input_halt_count+1'b1;
   if(event_data_out_channel_halt)output_halt_count<=output_halt_count+1'b1;
   if(event_status_channel_halt)status_halt_count<=status_halt_count+1'b1;
   if(active)begin
    job_timer<=job_timer+1'b1;
    if(loading)ingest_timer<=ingest_timer+1'b1;
    if(status_word&&!status_seen)begin status_seen<=1;status_exponent<=fft_m_axis_status_tdata;end
   end
   // A fault wins the final input/result/status and commit races. Local reset
   // affects only the FFT core and the currently reserved private result bank.
   if(fail_now)begin
    store_abort<=reserved;reserved<=0;drop_count<=drop_count+1'b1;sticky_gap<=1;
    if(deadline)timeout_count<=timeout_count+1'b1;
    else if(protocol_fault)protocol_error_count<=protocol_error_count+1'b1;
    last_job_cycles<=job_timer+1'b1;
    if(loading)last_ingest_cycles<=ingest_timer+1'b1;
    need_drain<=loading&&!frame_abort;state<=RESET_FFT;reset_timer<=0;status_seen<=0;
   end else case(state)
    IDLE: if(s_valid&&!abort&&!frame_abort)begin
     capture_id<=s_capture_id;first_raw<=s_first_raw;frame_seq<=s_frame_seq;config_id<=s_config_id;
     source_flags<=s_flags;bound_flags<=s_flags|(sticky_gap?8'h04:8'h00);
     ingest_timer<=1;job_timer<=1;input_index<=0;reserved<=0;status_seen<=0;status_exponent<=0;
     if(first_bad)begin
      drop_count<=drop_count+1'b1;protocol_error_count<=protocol_error_count+1'b1;sticky_gap<=1;
      state<=RESET_FFT;reset_timer<=0;need_drain<=1;
     end else state<=RESERVE;
    end
    RESERVE: if(reserve_ready)begin reserved<=1;state<=CONFIGURE;end
    CONFIGURE: if(fft_s_axis_config_tready)state<=LOAD;
    LOAD: if(s_valid&&s_ready)begin
     if(input_index==4095)begin last_ingest_cycles<=ingest_timer+1'b1;state<=WAIT_RESULT;end
     else input_index<=input_index+1'b1;
    end
    WAIT_RESULT: if(collect_done&&status_available)begin
     store_commit<=1;reserved<=0;completed_count<=completed_count+1'b1;
     last_job_cycles<=job_timer+1'b1;sticky_gap<=0;state<=IDLE;status_seen<=0;
    end
    RESET_FFT: begin
     if(frame_abort)need_drain<=0;
     if(reset_timer==RESET_HOLD-1)begin
      state<=need_drain&&!frame_abort?DRAIN:IDLE;reset_timer<=0;
     end else reset_timer<=reset_timer+1;
    end
    DRAIN: begin
     // Never consume the first beat of a new frame while discarding an old one.
     if(frame_abort||abort||(s_valid&&!same_identity))state<=IDLE;
     else if(s_valid&&s_ready&&s_last)state<=IDLE;
    end
    default: state<=IDLE;
   endcase
  end
 end
endmodule
