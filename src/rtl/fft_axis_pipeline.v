// Recoverable FFT job control and immutable complete spectra to protocol v1.
// Native FFT AXIS ports bind the separately generated AMD IP. abort affects
// only an uncommitted FFT job; packet_abort needs coordinated downstream DMA
// reset and explicitly releases the packet already owned by that consumer.
// The local reservation request may be cancelled before its handshake.
// Default ingest budget reserves ten clocks for frame_window READY/prefetch.
module fft_axis_pipeline #(
 parameter integer INGEST_CYCLES=49990, JOB_CYCLES=5000000, RESET_CYCLES=2, SHARED_STORE=0
)(input wire clk,rst,abort,frame_abort,
 input wire store_mode_valid,store_stopped,input wire[1:0]store_mode_select,
 output wire store_mode_ready,store_mode_rejected,output wire[1:0]store_mode_active,
 input wire store_flush_req,dma_spectrum_reset_done,input wire[31:0]dma_spectrum_reset_seq,
 output wire store_flush_pending,store_flush_done,store_flush_timeout,output wire[31:0]store_flush_request_seq,
 input wire raw_valid,output wire raw_ready,input wire signed[23:0]raw_data,
 input wire[63:0]raw_capture_id,raw_index,input wire[31:0]raw_config_id,input wire[7:0]raw_flags,
 output wire raw_grid_meta_valid,output wire[63:0]raw_grid_meta_capture,raw_grid_meta_seq,raw_grid_meta_raw,
 output wire[31:0]raw_grid_meta_config,
 output wire[63:0]raw_candidate_count,raw_completed_count,raw_drop_count,raw_invalid_count,
 output wire pipeline_busy,store_idle,core_clock_enable,analysis_pending,
 input wire s_valid,output wire s_ready,input wire signed[23:0]s_data,
 input wire[63:0]s_capture_id,s_first_raw,s_frame_seq,input wire[31:0]s_config_id,
 input wire[7:0]s_flags,input wire[11:0]s_index,input wire s_last,
 output wire[7:0]fft_s_axis_config_tdata,output wire fft_s_axis_config_tvalid,input wire fft_s_axis_config_tready,
 output wire[47:0]fft_s_axis_data_tdata,output wire fft_s_axis_data_tvalid,input wire fft_s_axis_data_tready,output wire fft_s_axis_data_tlast,
 input wire[47:0]fft_m_axis_data_tdata,input wire[23:0]fft_m_axis_data_tuser,input wire fft_m_axis_data_tvalid,output wire fft_m_axis_data_tready,input wire fft_m_axis_data_tlast,
 input wire[7:0]fft_m_axis_status_tdata,input wire fft_m_axis_status_tvalid,output wire fft_m_axis_status_tready,
 output wire fft_aresetn,input wire event_tlast_missing,event_tlast_unexpected,event_data_in_channel_halt,event_data_out_channel_halt,event_status_channel_halt,
 output wire[63:0]completed_count,drop_count,timeout_count,protocol_error_count,input_halt_count,output_halt_count,status_halt_count,
 output wire[31:0]last_ingest_cycles,last_job_cycles,
 input wire packet_abort,
 output wire m_axis_tvalid,input wire m_axis_tready,
 output wire[63:0]m_axis_tdata,output wire[7:0]m_axis_tkeep,output wire m_axis_tlast,
 output wire[63:0]store_completed_count,store_drop_count,store_invalid_count,
 output wire[63:0]packet_completed_count,packet_error_count);
 reg job_active,packet_owned;
 wire reserve_valid,reserve_ready,store_commit,store_abort,result_valid,result_ready,result_last,collect_done,collect_error;
 wire[63:0]reserve_capture_id,reserve_first_raw,reserve_frame_seq;
 wire[31:0]reserve_config_id;wire[7:0]reserve_flags,result_blk_exp,collect_blk_exp;
 wire signed[23:0]result_real,result_imag;wire[11:0]result_index;
 wire packet_valid,packet_ready,packet_done,packet_error,read_en,read_valid;
 wire[15:0]packet_type,packet_flags;wire[63:0]packet_capture_id,packet_seq,packet_first_raw;
 wire[31:0]packet_config_id,read_data;wire[7:0]packet_blk_exp;wire[12:0]read_addr;
 fft_scheduler #(.INGEST_CYCLES(INGEST_CYCLES),.JOB_CYCLES(JOB_CYCLES),.RESET_CYCLES(RESET_CYCLES)) scheduler(
  .clk(clk),
  .rst(rst),
  .abort(abort||(SHARED_STORE&&store_flush_req)),
  .frame_abort(frame_abort),
  .s_valid(s_valid&&(!SHARED_STORE||(store_mode_active==0&&!store_flush_pending&&!store_flush_req))),
  .s_ready(s_ready),
  .s_data(s_data),
  .s_capture_id(s_capture_id),
  .s_first_raw(s_first_raw),
  .s_frame_seq(s_frame_seq),
  .s_config_id(s_config_id),
  .s_flags(s_flags),
  .s_index(s_index),
  .s_last(s_last),
  .fft_s_axis_config_tdata(fft_s_axis_config_tdata),
  .fft_s_axis_config_tvalid(fft_s_axis_config_tvalid),
  .fft_s_axis_config_tready(fft_s_axis_config_tready),
  .fft_s_axis_data_tdata(fft_s_axis_data_tdata),
  .fft_s_axis_data_tvalid(fft_s_axis_data_tvalid),
  .fft_s_axis_data_tready(fft_s_axis_data_tready),
  .fft_s_axis_data_tlast(fft_s_axis_data_tlast),
  .fft_m_axis_data_tdata(fft_m_axis_data_tdata),
  .fft_m_axis_data_tuser(fft_m_axis_data_tuser),
  .fft_m_axis_data_tvalid(fft_m_axis_data_tvalid),
  .fft_m_axis_data_tready(fft_m_axis_data_tready),
  .fft_m_axis_data_tlast(fft_m_axis_data_tlast),
  .fft_m_axis_status_tdata(fft_m_axis_status_tdata),
  .fft_m_axis_status_tvalid(fft_m_axis_status_tvalid),
  .fft_m_axis_status_tready(fft_m_axis_status_tready),
  .fft_aresetn(fft_aresetn),
  .event_tlast_missing(event_tlast_missing),
  .event_tlast_unexpected(event_tlast_unexpected),
  .event_data_in_channel_halt(event_data_in_channel_halt),
  .event_data_out_channel_halt(event_data_out_channel_halt),
  .event_status_channel_halt(event_status_channel_halt),
  .reserve_valid(reserve_valid),
  .reserve_ready(reserve_ready),
  .reserve_capture_id(reserve_capture_id),
  .reserve_first_raw(reserve_first_raw),
  .reserve_frame_seq(reserve_frame_seq),
  .reserve_config_id(reserve_config_id),
  .reserve_flags(reserve_flags),
  .store_commit(store_commit),
  .store_abort(store_abort),
  .result_valid(result_valid),
  .result_ready(result_ready),
  .result_real(result_real),
  .result_imag(result_imag),
  .result_index(result_index),
  .result_blk_exp(result_blk_exp),
  .result_last(result_last),
  .collect_done(collect_done),
  .collect_error(collect_error),
  .collect_blk_exp(collect_blk_exp),
  .completed_count(completed_count),
  .drop_count(drop_count),
  .timeout_count(timeout_count),
  .protocol_error_count(protocol_error_count),
  .input_halt_count(input_halt_count),
  .output_halt_count(output_halt_count),
  .status_halt_count(status_halt_count),
  .last_ingest_cycles(last_ingest_cycles),
  .last_job_cycles(last_job_cycles));
 generate if(SHARED_STORE==0)begin:legacy_store
 fft_result_store store(
  .clk(clk),
  .rst(rst),
  .reserve_valid(reserve_valid),
  .reserve_ready(reserve_ready),
  .reserve_capture_id(reserve_capture_id),
  .reserve_first_raw(reserve_first_raw),
  .reserve_frame_seq(reserve_frame_seq),
  .reserve_config_id(reserve_config_id),
  .reserve_flags(reserve_flags),
  .abort(store_abort),
  .commit(store_commit),
  .result_valid(result_valid),
  .result_ready(result_ready),
  .result_real(result_real),
  .result_imag(result_imag),
  .result_index(result_index),
  .result_blk_exp(result_blk_exp),
  .result_last(result_last),
  .collect_done(collect_done),
  .collect_error(collect_error),
  .collect_blk_exp(collect_blk_exp),
  .packet_valid(packet_valid),
  .packet_ready(packet_ready),
  .packet_type(packet_type),
  .packet_flags(packet_flags),
  .packet_capture_id(packet_capture_id),
  .packet_seq(packet_seq),
  .packet_first_raw(packet_first_raw),
  .packet_config_id(packet_config_id),
  .packet_blk_exp(packet_blk_exp),
  .read_en(read_en),
  .read_addr(read_addr),
  .read_valid(read_valid),
  .read_data(read_data),
  .packet_done(packet_done),
  .completed_count(store_completed_count),
  .drop_count(store_drop_count),
  .invalid_count(store_invalid_count));
 assign store_mode_ready=0;assign store_mode_rejected=0;assign store_mode_active=0;
 assign store_flush_pending=0;assign store_flush_done=0;assign store_flush_timeout=0;assign store_flush_request_seq=0;
 assign raw_grid_meta_valid=0;assign raw_grid_meta_capture=0;assign raw_grid_meta_config=0;assign raw_grid_meta_seq=0;assign raw_grid_meta_raw=0;
 assign raw_ready=0;assign raw_candidate_count=0;assign raw_completed_count=0;assign raw_drop_count=0;assign raw_invalid_count=0;
 assign store_idle=!packet_owned&&!packet_valid&&!collect_done&&!result_ready;
 end else begin:shared_store
 spectrum_shared_store store(
  .mode_valid(store_mode_valid),.mode_select(store_mode_select),.stopped(store_stopped),
  .mode_ready(store_mode_ready),.mode_rejected(store_mode_rejected),.mode_active(store_mode_active),.banks_idle(store_idle),
  .flush_req(store_flush_req),.dma_reset_done(dma_spectrum_reset_done),.dma_reset_seq(dma_spectrum_reset_seq),
  .flush_pending(store_flush_pending),.flush_done(store_flush_done),.flush_timeout(store_flush_timeout),.flush_request_seq(store_flush_request_seq),
  .raw_valid(raw_valid),.raw_ready(raw_ready),.raw_data(raw_data),.raw_capture_id(raw_capture_id),.raw_index(raw_index),
  .raw_config_id(raw_config_id),.raw_flags(raw_flags),.raw_analysis_reset(abort),
   .grid_meta_valid(raw_grid_meta_valid),.grid_meta_capture(raw_grid_meta_capture),.grid_meta_config(raw_grid_meta_config),.grid_meta_seq(raw_grid_meta_seq),.grid_meta_raw(raw_grid_meta_raw),
 .raw_candidate_count(raw_candidate_count),.raw_completed_count(raw_completed_count),.raw_drop_count(raw_drop_count),.raw_invalid_count(raw_invalid_count),
  .clk(clk),
  .rst(rst),
  .reserve_valid(reserve_valid),
  .reserve_ready(reserve_ready),
  .reserve_capture_id(reserve_capture_id),
  .reserve_first_raw(reserve_first_raw),
  .reserve_frame_seq(reserve_frame_seq),
  .reserve_config_id(reserve_config_id),
  .reserve_flags(reserve_flags),
  .abort(store_abort),
  .commit(store_commit),
  .result_valid(result_valid),
  .result_ready(result_ready),
  .result_real(result_real),
  .result_imag(result_imag),
  .result_index(result_index),
  .result_blk_exp(result_blk_exp),
  .result_last(result_last),
  .collect_done(collect_done),
  .collect_error(collect_error),
  .collect_blk_exp(collect_blk_exp),
  .packet_valid(packet_valid),
  .packet_ready(packet_ready),
  .packet_type(packet_type),
  .packet_flags(packet_flags),
  .packet_capture_id(packet_capture_id),
  .packet_seq(packet_seq),
  .packet_first_raw(packet_first_raw),
  .packet_config_id(packet_config_id),
  .packet_blk_exp(packet_blk_exp),
  .read_en(read_en),
  .read_addr(read_addr),
  .read_valid(read_valid),
  .read_data(read_data),
  .packet_done(packet_done),
  .completed_count(store_completed_count),
  .drop_count(store_drop_count),
  .invalid_count(store_invalid_count));
 end endgenerate
 axis_packetizer packetizer(
  .clk(clk),
  .rst(rst),
  .abort(packet_abort||(SHARED_STORE&&store_flush_pending&&dma_spectrum_reset_done&&dma_spectrum_reset_seq==store_flush_request_seq)),
  .packet_valid(packet_valid),
  .packet_ready(packet_ready),
  .packet_type(packet_type),
  .packet_flags(packet_flags),
  .packet_capture_id(packet_capture_id),
  .packet_seq(packet_seq),
  .packet_first_raw(packet_first_raw),
  .packet_config_id(packet_config_id),
  .packet_blk_exp(packet_blk_exp),
  .packet_done(packet_done),
  .packet_error(packet_error),
  .read_en(read_en),
  .read_addr(read_addr),
  .read_valid(read_valid),
  .read_data(read_data),
  .m_axis_tvalid(m_axis_tvalid),
  .m_axis_tready(m_axis_tready),
  .m_axis_tdata(m_axis_tdata),
  .m_axis_tkeep(m_axis_tkeep),
  .m_axis_tlast(m_axis_tlast),
  .error_count(packet_error_count),
  .completed_count(packet_completed_count));

 always @(posedge clk)begin
  if(rst)begin job_active<=0;packet_owned<=0;end
  else begin
   if(reserve_valid&&reserve_ready)job_active<=1;
   if(store_commit||store_abort)job_active<=0;
   if(packet_valid&&packet_ready)packet_owned<=1;
   if(packet_done)packet_owned<=0;
  end
 end
 // Separate accepted FFT work from published packets and a partial RAW bank.
 // History may clear only after upstream/window work and this signal drain.
 assign analysis_pending=job_active||reserve_valid||!fft_aresetn||fft_m_axis_data_tvalid||fft_m_axis_status_tvalid;
 assign pipeline_busy=job_active||packet_owned||!store_idle;
 // A reservation precedes CONFIGURE. Retain clocks through all native valid
 // tails and the complete synchronous reset interval, including local faults.
 assign core_clock_enable=rst||!fft_aresetn||reserve_valid||job_active||fft_m_axis_data_tvalid||fft_m_axis_status_tvalid;
endmodule


