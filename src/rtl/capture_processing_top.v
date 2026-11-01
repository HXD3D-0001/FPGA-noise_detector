// Source/frontend plus processing integration; optional audio clock service. No PS ABI.
// Requests are one-cycle proc_clk pulses. DMA idle/reset claims remain external.
// Configuration is latched only on an accepted START and held for the capture.
module capture_processing_top #(
 parameter integer CLOCK_MONITOR_ENABLE=0,CLOCK_COMMAND_TIMEOUT_CYCLES=250000000,CLK_HZ=50000000,FFT_CLOCK_GATE=0,DEBUG_INGEST_STALL=0,
 FIFO_DEPTH=1024,RESET_HOLD_CYCLES=8,ADC_WARMUP_CYCLES=100000000,
 START_TIMEOUT_CYCLES=250000000,DRAIN_TIMEOUT_CYCLES=5000000,
 BCK_TIMEOUT_CYCLES=50000,SLOT_TIMEOUT_CYCLES=500000,TEST_TIMEOUT_CYCLES=500000,
 parameter SINE_FILE="generated/pattern_sine.mem",
 parameter FIR0_FILE="generated/fir_stage1.mem",FIR1_FILE="generated/fir_stage2.mem",
 parameter FIR2_FILE="generated/fir_stage3.mem",HANN_FILE="generated/hann4096.mem"
)(
 input wire proc_clk,bclk,rst,start_req,stop_req,requested_test_mode,
 input wire mclk_ready_proc,clock_fault_latched,clock_restart_pending,
 input wire clock_command_req,input wire[1:0]clock_command_kind,input wire[31:0]clock_command_seq,
 output wire clock_command_ack,clock_command_error,clock_control_busy,
 output wire[31:0]clock_command_ack_seq,output wire[3:0]clock_command_reason,
 output wire restart_clocks,clear_clock_fault,active_test_mode,
 input wire clock_snapshot_req,input wire[31:0]clock_snapshot_request_seq,
 output wire clock_snapshot_ack,clock_snapshot_error,clock_snapshot_valid,
 output wire[31:0]clock_snapshot_ack_seq,output wire[511:0]clock_snapshot_data,
 output wire[63:0]clock_snapshot_age,
 input wire[63:0]next_capture_id,origin_raw,input wire[31:0]requested_config_id,
 input wire channel_select,lrck,sdata,input wire[1:0]pattern_id,
 // True only after the external DMA owner confirms no in-flight receive.
 input wire dma_idle,dma_abort,
 output wire start_ready,stop_complete,running,frontend_busy,command_rejected,
 output wire[3:0]frontend_state,fault_reason,output wire[63:0]active_capture_id,fault_count,
 output reg[31:0]active_config_id,
 output wire[63:0]adc_count,good_slot_count,test_count,
 output wire history_clear,cleanup_pending,
 input wire clear_energy_records,
 input wire test_fft_input_stall,
 input wire dma_spectrum_abort,dma_pcm_abort,
 input wire mode_valid,input wire[1:0]mode_select,
 output wire mode_ready,mode_rejected,output wire[1:0]mode_active,
 input wire spectrum_flush_req,dma_spectrum_reset_done,input wire[31:0]dma_spectrum_reset_seq,
 output wire spectrum_flush_pending,spectrum_flush_done,spectrum_flush_timeout,output wire[31:0]spectrum_flush_request_seq,
 output wire[3:0]grid_meta_valid,
 output wire[255:0]grid_meta_capture,grid_meta_seq,grid_meta_raw,
 output wire[127:0]grid_meta_config,
 output wire processing_busy,drained,history_quiescent,
 output wire[63:0]fork_fft_drops,fork_pcm_drops,fork_energy_drops,
 output wire[31:0]fork_fft_highwater,fork_pcm_highwater,fork_energy_highwater,
 output wire[63:0]frame_candidates,frame_drops,frame_rejected,window_saturations,
 output wire[63:0]store_completed,store_drops,store_invalid,fft_packet_errors,fft_packets,
 output wire[63:0]pcm_invalid,pcm_packet_errors,pcm_packets,energy_discontinuities,energy_config_errors,
 output wire[31:0]energy_highwater,
 output wire[31:0]energy_occupancy,
 output wire[63:0]energy_flush_count,pcm_flush_count,
 output wire analysis_ready,source_position_valid,
 output wire[63:0]latest_raw,
 output wire[63:0]fir0_saturations,fir1_saturations,fir2_saturations,
 output wire[63:0]raw_candidates,raw_completed,raw_drops,raw_invalid,
 output wire fft_axis_tvalid,input wire fft_axis_tready,output wire[63:0]fft_axis_tdata,
 output wire[7:0]fft_axis_tkeep,output wire fft_axis_tlast,
 output wire pcm_axis_tvalid,input wire pcm_axis_tready,output wire[63:0]pcm_axis_tdata,
 output wire[7:0]pcm_axis_tkeep,output wire pcm_axis_tlast,
 output wire energy_valid,input wire energy_ready,output wire[63:0]energy_seq,energy_capture,energy_first_raw,energy_value,
 output wire[31:0]energy_config,output wire[7:0]energy_flags,
 output wire[63:0]decimation_saturations,
 output wire[63:0]fft_completed,fft_drops,fft_timeouts,fft_errors,pcm_candidates,pcm_drops,energy_candidates,energy_drops,
 output wire[31:0]last_ingest_cycles,last_job_cycles);

 // Both coordinator and frontend release reset after three proc edges.
 (* ASYNC_REG="TRUE" *)reg[2:0]reset_pipe;
 always@(posedge proc_clk or posedge rst)
  if(rst)reset_pipe<=3'b111;else reset_pipe<={reset_pipe[1:0],1'b0};
 wire local_rst=rst|reset_pipe[2];
 wire sample_valid,sample_ready,sample_reset,frontend_rejected,frontend_fault_event;
 wire signed[23:0]sample_data;wire[63:0]sample_capture,sample_index;wire[7:0]sample_flags;
 reg saved_channel,start_pulse;reg[1:0]saved_pattern;
 // WAIT_HISTORY -> CLEAR_HISTORY -> WAIT_OWNERS -> CLEAN.
 // No timeout is converted into a fabricated success or DMA acknowledgement.
 reg[1:0]cleanup;
 wire lifecycle_stopped=frontend_state==0&&cleanup==0&&energy_occupancy==0&&dma_idle;
 assign stop_complete=!local_rst&&lifecycle_stopped&&drained;
 wire clock_unavailable=CLOCK_MONITOR_ENABLE ? (!mclk_ready_proc||clock_fault_latched) : 1'b0;
 wire clock_block=CLOCK_MONITOR_ENABLE ? (clock_command_req||clock_control_busy) : 1'b0;
 assign start_ready=!clock_block&&(requested_test_mode||!clock_unavailable)&&stop_complete&&mode_ready&&!mode_valid&&requested_config_id!=0&&
                    next_capture_id!=0&&next_capture_id>active_capture_id;
 wire accepted_start=start_req&&!stop_req&&start_ready;
 wire fault_cancel=!local_rst&&frontend_fault_event&&!stop_req&&(frontend_state==5||frontend_state==7);
 assign history_clear=start_pulse||fault_cancel||cleanup==2;
 assign cleanup_pending=cleanup!=0;
 assign command_rejected=frontend_rejected||(!local_rst&&start_req&&!accepted_start);
 always@(posedge proc_clk or posedge local_rst)begin
  if(local_rst)begin cleanup<=0;active_config_id<=0;saved_channel<=0;saved_pattern<=0;start_pulse<=0;end
  else begin
   start_pulse<=accepted_start;
   if(accepted_start)begin active_config_id<=requested_config_id;saved_channel<=channel_select;saved_pattern<=pattern_id;end
   if(fault_cancel)cleanup<=3;
   else if(stop_req&&frontend_state!=0)cleanup<=1;
   else case(cleanup)
    1:if(history_quiescent)cleanup<=2;
    2:cleanup<=3;
    3:if(drained&&energy_occupancy==0&&dma_idle)cleanup<=0;
    default:cleanup<=0;
   endcase
  end
 end
 generate if(CLOCK_MONITOR_ENABLE)begin:clock_control
  audio_clock_service #(.TIMEOUT_CYCLES(CLOCK_COMMAND_TIMEOUT_CYCLES)) service(
   .clk(proc_clk),.rst(local_rst),.allow_commands(stop_complete&&!mode_valid&&!stop_req),
   .mclk_ready(mclk_ready_proc),.clock_fault(clock_fault_latched),.restart_pending(clock_restart_pending),
   .command_req(clock_command_req),.command_kind(clock_command_kind),.command_seq(clock_command_seq),
   .command_ack(clock_command_ack),.command_error(clock_command_error),.command_ack_seq(clock_command_ack_seq),
   .command_reason(clock_command_reason),.busy(clock_control_busy),.restart_clocks(restart_clocks),.clear_clock_fault(clear_clock_fault),
   .snapshot_req(clock_snapshot_req),.snapshot_request_seq(clock_snapshot_request_seq),
   .capture_id(active_capture_id),.adc_count(adc_count),.test_count(test_count),.latest_raw(latest_raw),
   .capture_fault_count(fault_count),.config_id(active_config_id),.capture_state(frontend_state),.capture_reason(fault_reason),
   .active_test(active_test_mode),.running(running),.start_ready(start_ready),.stop_complete(stop_complete),.position_valid(source_position_valid),
   .snapshot_ack(clock_snapshot_ack),.snapshot_error(clock_snapshot_error),.snapshot_valid(clock_snapshot_valid),
   .snapshot_ack_seq(clock_snapshot_ack_seq),.snapshot_data(clock_snapshot_data),.snapshot_age(clock_snapshot_age));
 end else begin:no_clock_control
  assign clock_command_ack=0;assign clock_command_error=0;assign clock_control_busy=0;assign clock_command_ack_seq=0;
  assign clock_command_reason=0;assign restart_clocks=0;assign clear_clock_fault=0;
  assign clock_snapshot_ack=0;assign clock_snapshot_error=0;assign clock_snapshot_valid=0;
  assign clock_snapshot_ack_seq=0;assign clock_snapshot_data=0;assign clock_snapshot_age=64'hffffffffffffffff;
 end endgenerate
 capture_frontend #(.CLOCK_MONITOR_ENABLE(CLOCK_MONITOR_ENABLE),.FIFO_DEPTH(FIFO_DEPTH),.CLK_HZ(CLK_HZ),.SAMPLE_HZ(48000),.SINE_FILE(SINE_FILE),
  .RESET_HOLD_CYCLES(RESET_HOLD_CYCLES),.ADC_WARMUP_CYCLES(ADC_WARMUP_CYCLES),
  .START_TIMEOUT_CYCLES(START_TIMEOUT_CYCLES),.DRAIN_TIMEOUT_CYCLES(DRAIN_TIMEOUT_CYCLES),
  .BCK_TIMEOUT_CYCLES(BCK_TIMEOUT_CYCLES),.SLOT_TIMEOUT_CYCLES(SLOT_TIMEOUT_CYCLES),.TEST_TIMEOUT_CYCLES(TEST_TIMEOUT_CYCLES)) frontend(
  .proc_clk(proc_clk),.bclk(bclk),.rst(rst),.mclk_ready_proc(mclk_ready_proc),.clock_fault_latched(clock_fault_latched),
  .active_test_mode(active_test_mode),.start_req(accepted_start),.stop_req(stop_req),
  .requested_test_mode(requested_test_mode),.next_capture_id(next_capture_id),
  .drain_done(cleanup==0&&drained&&energy_occupancy==0&&dma_idle),
  .channel_select(saved_channel),.pattern_id(saved_pattern),.origin_raw(origin_raw),.lrck(lrck),.sdata(sdata),
  .running(running),.busy(frontend_busy),.command_rejected(frontend_rejected),.active_capture_id(active_capture_id),
  .state(frontend_state),.fault_reason(fault_reason),.fault_count(fault_count),.sample_reset(sample_reset),.source_fault_event(frontend_fault_event),
  .m_valid(sample_valid),.m_ready(sample_ready),.m_data(sample_data),.m_capture_id(sample_capture),.m_raw_index(sample_index),.m_flags(sample_flags),
  .adc_count(adc_count),.good_slot_count(good_slot_count),.test_count(test_count));
 sample_processing_core #(.CLK_HZ(CLK_HZ),.FFT_CLOCK_GATE(FFT_CLOCK_GATE),.DEBUG_INGEST_STALL(DEBUG_INGEST_STALL),
  .FIR0_FILE(FIR0_FILE),.FIR1_FILE(FIR1_FILE),.FIR2_FILE(FIR2_FILE),.HANN_FILE(HANN_FILE)) processing(
  // Snapshot invalidation is atomic with the frontend capture/config latch.
  // Only history clear is delayed; start has no combinational ready path.
  .clk(proc_clk),.rst(local_rst),.start(accepted_start),.clear_history(history_clear),
  .dma_abort(dma_abort),.s_valid(sample_valid),.s_ready(sample_ready),.s_data(sample_data),
  .s_capture_id(sample_capture),.s_raw_index(sample_index),.s_flags(sample_flags),.clear_energy_records(clear_energy_records),
  .test_fft_input_stall(test_fft_input_stall),.dma_spectrum_abort(dma_spectrum_abort),.dma_pcm_abort(dma_pcm_abort),.mode_valid(mode_valid),
  .stopped(lifecycle_stopped),.mode_select(mode_select),.mode_ready(mode_ready),.mode_rejected(mode_rejected),
  .mode_active(mode_active),.spectrum_flush_req(spectrum_flush_req),.dma_spectrum_reset_done(dma_spectrum_reset_done),.dma_spectrum_reset_seq(dma_spectrum_reset_seq),
  .spectrum_flush_pending(spectrum_flush_pending),.spectrum_flush_done(spectrum_flush_done),.spectrum_flush_timeout(spectrum_flush_timeout),.spectrum_flush_request_seq(spectrum_flush_request_seq),
  .grid_meta_valid(grid_meta_valid),.grid_meta_capture(grid_meta_capture),.grid_meta_config(grid_meta_config),.grid_meta_seq(grid_meta_seq),.grid_meta_raw(grid_meta_raw),.processing_busy(processing_busy),.drained(drained),.history_quiescent(history_quiescent),.fork_fft_drops(fork_fft_drops),
  .fork_pcm_drops(fork_pcm_drops),.fork_energy_drops(fork_energy_drops),.fork_fft_highwater(fork_fft_highwater),.fork_pcm_highwater(fork_pcm_highwater),
  .fork_energy_highwater(fork_energy_highwater),.frame_candidates(frame_candidates),.frame_drops(frame_drops),.frame_rejected(frame_rejected),
  .window_saturations(window_saturations),.store_completed(store_completed),.store_drops(store_drops),.store_invalid(store_invalid),
  .fft_packet_errors(fft_packet_errors),.fft_packets(fft_packets),.pcm_invalid(pcm_invalid),.pcm_packet_errors(pcm_packet_errors),
  .pcm_packets(pcm_packets),.energy_discontinuities(energy_discontinuities),.energy_config_errors(energy_config_errors),.energy_highwater(energy_highwater),
  .energy_occupancy(energy_occupancy),.energy_flush_count(energy_flush_count),.pcm_flush_count(pcm_flush_count),.analysis_ready(analysis_ready),
  .source_position_valid(source_position_valid),.latest_raw(latest_raw),.fir0_saturations(fir0_saturations),.fir1_saturations(fir1_saturations),
  .fir2_saturations(fir2_saturations),.raw_candidates(raw_candidates),.raw_completed(raw_completed),.raw_drops(raw_drops),
  .raw_invalid(raw_invalid),.config_id(active_config_id),.fft_axis_tvalid(fft_axis_tvalid),.fft_axis_tready(fft_axis_tready),
  .fft_axis_tdata(fft_axis_tdata),.fft_axis_tkeep(fft_axis_tkeep),.fft_axis_tlast(fft_axis_tlast),.pcm_axis_tvalid(pcm_axis_tvalid),
  .pcm_axis_tready(pcm_axis_tready),.pcm_axis_tdata(pcm_axis_tdata),.pcm_axis_tkeep(pcm_axis_tkeep),.pcm_axis_tlast(pcm_axis_tlast),
  .energy_valid(energy_valid),.energy_ready(energy_ready),.energy_seq(energy_seq),.energy_capture(energy_capture),
  .energy_first_raw(energy_first_raw),.energy_value(energy_value),.energy_config(energy_config),.energy_flags(energy_flags),
  .decimation_saturations(decimation_saturations),.fft_completed(fft_completed),.fft_drops(fft_drops),.fft_timeouts(fft_timeouts),
  .fft_errors(fft_errors),.pcm_candidates(pcm_candidates),.pcm_drops(pcm_drops),.energy_candidates(energy_candidates),
  .energy_drops(energy_drops),.last_ingest_cycles(last_ingest_cycles),.last_job_cycles(last_job_cycles));
endmodule
