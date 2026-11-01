// Closed TEST-only control service and actual FFT processing datapath.
// All control service signals bind directly to same-named control_axi pins.
module monitor_service_top #(
 parameter integer CLK_HZ=50000000,FFT_CLOCK_GATE=1,SERVICE_TIMEOUT_TICKS=4999990,
 parameter[63:0]BOOT_ORIGIN=0,
 parameter SINE_FILE="generated/pattern_sine.mem",
 parameter FIR0_FILE="generated/fir_stage1.mem",FIR1_FILE="generated/fir_stage2.mem",
 parameter FIR2_FILE="generated/fir_stage3.mem",HANN_FILE="generated/hann4096.mem"
)(
(* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 clk CLK", X_INTERFACE_PARAMETER="ASSOCIATED_BUSIF m_spectrum_axis:m_pcm_axis, ASSOCIATED_RESET aresetn, FREQ_HZ 50000000" *)
 input wire clk,
(* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 aresetn RST", X_INTERFACE_PARAMETER="POLARITY ACTIVE_LOW" *)
 input wire aresetn,
 output wire m_spectrum_axis_tvalid,input wire m_spectrum_axis_tready,output wire[63:0]m_spectrum_axis_tdata,
 output wire[7:0]m_spectrum_axis_tkeep,output wire m_spectrum_axis_tlast,
 output wire m_pcm_axis_tvalid,input wire m_pcm_axis_tready,output wire[63:0]m_pcm_axis_tdata,
 output wire[7:0]m_pcm_axis_tkeep,output wire m_pcm_axis_tlast,
 output wire [31:0] state_i,status_i,irq_events_i,
 input wire command_req,input wire [2:0] command_kind,input wire [31:0] command_seq,
 output wire command_ack,command_error,output wire [31:0] command_ack_seq,
 input wire commit_req,input wire [31:0] commit_mode,commit_config_id,commit_seq,
 output wire commit_ack,commit_error,output wire [31:0] commit_ack_seq,
 
 input wire snapshot_req,input wire [31:0] snapshot_request_seq,
 output wire snapshot_ack,snapshot_error,snapshot_invalidate,output wire [31:0] snapshot_ack_seq,
 // Counters: capture/raw/rx_drop/saturation/fft_drop/pcm_drop/protocol/clock,
 // adc_count/adc_clip/fir0/fir1/fir2/energy_drop/raw_drop/energy_highwater.
 output wire [1023:0] snapshot_counters_i,
 output wire[63:0]snapshot_energy_flush_i,
 output wire[63:0]snapshot_pcm_flush_i,
 output wire[255:0]snapshot_sequences_i,
 output wire [31:0] snapshot_source_id_i,output wire snapshot_position_valid_i,
 output wire [63:0] snapshot_latest_raw_i,snapshot_origin_raw_i,
 input wire recovery_req,output wire recovery_ack,
 output wire energy_valid,energy_invalidate,input wire energy_ready,output wire [31:0] energy_count_i,
 output wire [63:0] energy_seq,energy_capture_id,energy_first_raw,energy_value,
 output wire [31:0] energy_config_id,output wire [7:0] energy_flags,
 output wire[31:0]dma_reset_request_seq_i,output wire[1:0]dma_reset_request_mask_i,output wire dma_reset_request_reset_i,
 input wire dma_reset_ack_valid,input wire[31:0]dma_reset_ack_seq,input wire[1:0]dma_reset_ack_mask,
 input wire flow_req,input wire[1:0]flow_mask,input wire[31:0]flow_seq,
 output wire flow_ack,flow_error,output wire[31:0]flow_ack_seq

);
 wire rst=!aresetn;
 wire enable,start,clear_history,clear_records,mode_valid,stopped,mode_ready,mode_rejected;
 wire[1:0]mode_select,bank_mode;
 wire spectrum_flush_req,spectrum_flush_pending,spectrum_flush_done,spectrum_flush_timeout;
 wire dma_spectrum_reset_done,dma_pcm_abort;wire[31:0]spectrum_flush_request_seq,dma_spectrum_reset_seq;
 wire spectrum_output_hold,pcm_output_hold,spectrum_valid,pcm_valid;
 assign m_spectrum_axis_tvalid=spectrum_valid&&!spectrum_output_hold;
 assign m_pcm_axis_tvalid=pcm_valid&&!pcm_output_hold;
 wire busy,drained,analysis_ready,source_position_valid;
 wire[63:0]capture_id,origin_raw,latest_raw;wire[31:0]config_id;
 wire[63:0]raw_count,source_drops,saturations,fft_completed,fft_drops,fft_timeouts,fft_errors;
 wire[63:0]pcm_candidates,pcm_drops,pcm_invalid,pcm_errors,pcm_packets,pcm_flush_count;
 wire[63:0]energy_candidates,energy_drops,energy_flush_count,energy_discontinuities,energy_config_errors;
 wire[63:0]frame_candidates,frame_drops,frame_rejected,window_saturations;
 wire[63:0]store_completed,store_drops,store_invalid,fft_packet_errors,fft_packets;
 wire[63:0]fork_fft_drops,fork_pcm_drops,fork_energy_drops,raw_candidates,raw_completed,raw_drops,raw_invalid;
 wire[63:0]fir0_saturations,fir1_saturations,fir2_saturations;
 wire[31:0]energy_highwater;
 wire[1023:0]counters_i={32'd0,energy_highwater,raw_drops,energy_drops,fir2_saturations,fir1_saturations,fir0_saturations,
  64'd0,64'd0,64'd0,(fft_errors+store_invalid+fft_packet_errors+pcm_invalid+pcm_errors+energy_config_errors+frame_rejected+raw_invalid),
  (pcm_drops+fork_pcm_drops),(frame_drops+fft_drops+fork_fft_drops),(saturations+window_saturations),source_drops,raw_count,capture_id};
 wire[255:0]sequences_i={raw_candidates,energy_candidates,pcm_candidates,frame_candidates};
 control_service #(.TIMEOUT_TICKS(SERVICE_TIMEOUT_TICKS),.BOOT_ORIGIN(BOOT_ORIGIN)) service(
  .flow_req(flow_req),.flow_mask(flow_mask),.flow_seq(flow_seq),.flow_ack(flow_ack),.flow_error(flow_error),.flow_ack_seq(flow_ack_seq),
  .clk(clk),.rst(rst),.command_req(command_req),.command_kind(command_kind),.command_seq(command_seq),
  .command_ack(command_ack),.command_error(command_error),.command_ack_seq(command_ack_seq),
  .commit_req(commit_req),.commit_mode(commit_mode),.commit_config_id(commit_config_id),.commit_seq(commit_seq),
  .commit_ack(commit_ack),.commit_error(commit_error),.commit_ack_seq(commit_ack_seq),
  .snapshot_req(snapshot_req),.snapshot_request_seq(snapshot_request_seq),.snapshot_ack(snapshot_ack),.snapshot_error(snapshot_error),
  .snapshot_invalidate(snapshot_invalidate),.snapshot_ack_seq(snapshot_ack_seq),.snapshot_counters_i(snapshot_counters_i),
  .snapshot_energy_flush_i(snapshot_energy_flush_i),.snapshot_pcm_flush_i(snapshot_pcm_flush_i),.snapshot_sequences_i(snapshot_sequences_i),
  .snapshot_source_id_i(snapshot_source_id_i),.snapshot_position_valid_i(snapshot_position_valid_i),
  .snapshot_latest_raw_i(snapshot_latest_raw_i),.snapshot_origin_raw_i(snapshot_origin_raw_i),
  .recovery_req(recovery_req),.recovery_ack(recovery_ack),.state_i(state_i),.status_i(status_i),.irq_events_i(irq_events_i),
  .dma_reset_request_seq_i(dma_reset_request_seq_i),.dma_reset_request_mask_i(dma_reset_request_mask_i),
  .dma_reset_request_reset_i(dma_reset_request_reset_i),.dma_reset_ack_valid(dma_reset_ack_valid),
  .dma_reset_ack_seq(dma_reset_ack_seq),.dma_reset_ack_mask(dma_reset_ack_mask),
  .drained(drained),.analysis_ready(analysis_ready),.mode_ready(mode_ready),.mode_rejected(mode_rejected),.mode_active(bank_mode),
  .mode_valid(mode_valid),.stopped(stopped),.mode_select(mode_select),.enable(enable),.start(start),.clear_history(clear_history),
  .clear_records(clear_records),.energy_invalidate(energy_invalidate),.spectrum_flush_req(spectrum_flush_req),
  .dma_spectrum_reset_done(dma_spectrum_reset_done),.dma_pcm_abort(dma_pcm_abort),
  .spectrum_output_hold(spectrum_output_hold),.pcm_output_hold(pcm_output_hold),
  .spectrum_flush_pending(spectrum_flush_pending),.spectrum_flush_done(spectrum_flush_done),
  .spectrum_flush_request_seq(spectrum_flush_request_seq),.dma_spectrum_reset_seq(dma_spectrum_reset_seq),
  .capture_id(capture_id),.origin_raw(origin_raw),.config_id(config_id),.raw_count(raw_count),.latest_raw(latest_raw),
  .energy_flush_count(energy_flush_count),.pcm_flush_count(pcm_flush_count),.source_position_valid(source_position_valid),.energy_occupancy(energy_count_i),
  .counters_i(counters_i),.sequences_i(sequences_i));
 top_pattern #(.CLK_HZ(CLK_HZ),.FFT_CLOCK_GATE(FFT_CLOCK_GATE),.SINE_FILE(SINE_FILE),
  .FIR0_FILE(FIR0_FILE),.FIR1_FILE(FIR1_FILE),.FIR2_FILE(FIR2_FILE),.HANN_FILE(HANN_FILE)) processing(
  .clk(clk),.rst(rst),.enable(enable),.start(start),.clear_history(clear_history),.dma_abort(1'b0),.test_fft_input_stall(1'b0),
  .clear_energy_records(clear_records),.dma_spectrum_abort(1'b0),.dma_pcm_abort(dma_pcm_abort),
  .mode_valid(mode_valid),.stopped(stopped),.mode_select(mode_select),.mode_ready(mode_ready),.mode_rejected(mode_rejected),.mode_active(bank_mode),
  .spectrum_flush_req(spectrum_flush_req),.dma_spectrum_reset_done(dma_spectrum_reset_done),.dma_spectrum_reset_seq(dma_spectrum_reset_seq),
  .spectrum_flush_pending(spectrum_flush_pending),.spectrum_flush_done(spectrum_flush_done),.spectrum_flush_timeout(spectrum_flush_timeout),
  .spectrum_flush_request_seq(spectrum_flush_request_seq),.processing_busy(busy),.drained(drained),
  .analysis_ready(analysis_ready),.source_position_valid(source_position_valid),.latest_raw(latest_raw),
  .capture_id(capture_id),.origin_raw(BOOT_ORIGIN),.config_id(config_id),.pattern_id(2'd3),
  .fft_axis_tvalid(spectrum_valid),.fft_axis_tready(m_spectrum_axis_tready&&!spectrum_output_hold),.fft_axis_tdata(m_spectrum_axis_tdata),
  .fft_axis_tkeep(m_spectrum_axis_tkeep),.fft_axis_tlast(m_spectrum_axis_tlast),
  .pcm_axis_tvalid(pcm_valid),.pcm_axis_tready(m_pcm_axis_tready&&!pcm_output_hold),.pcm_axis_tdata(m_pcm_axis_tdata),.pcm_axis_tkeep(m_pcm_axis_tkeep),.pcm_axis_tlast(m_pcm_axis_tlast),
  .energy_valid(energy_valid),.energy_ready(energy_ready),.energy_seq(energy_seq),.energy_capture(energy_capture_id),
  .energy_first_raw(energy_first_raw),.energy_value(energy_value),.energy_config(energy_config_id),.energy_flags(energy_flags),
  .energy_occupancy(energy_count_i),.energy_highwater(energy_highwater),.energy_flush_count(energy_flush_count),.pcm_flush_count(pcm_flush_count),
  .raw_count(raw_count),.source_drops(source_drops),.decimation_saturations(saturations),
  .fir0_saturations(fir0_saturations),.fir1_saturations(fir1_saturations),.fir2_saturations(fir2_saturations),
  .fft_completed(fft_completed),.fft_drops(fft_drops),.fft_timeouts(fft_timeouts),.fft_errors(fft_errors),
  .pcm_candidates(pcm_candidates),.pcm_drops(pcm_drops),.pcm_invalid(pcm_invalid),.pcm_packet_errors(pcm_errors),.pcm_packets(pcm_packets),
  .energy_candidates(energy_candidates),.energy_drops(energy_drops),.energy_discontinuities(energy_discontinuities),.energy_config_errors(energy_config_errors),
  .frame_candidates(frame_candidates),.frame_drops(frame_drops),.frame_rejected(frame_rejected),.window_saturations(window_saturations),
  .store_completed(store_completed),.store_drops(store_drops),.store_invalid(store_invalid),.fft_packet_errors(fft_packet_errors),.fft_packets(fft_packets),
  .fork_fft_drops(fork_fft_drops),.fork_pcm_drops(fork_pcm_drops),.fork_energy_drops(fork_energy_drops),
  .raw_candidates(raw_candidates),.raw_completed(raw_completed),.raw_drops(raw_drops),.raw_invalid(raw_invalid));
endmodule
