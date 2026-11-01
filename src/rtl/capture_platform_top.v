`default_nettype none
// Independent CAP1/DAT1/CTR1; sole owner and physical audio boundary.
// Candidate only: board constraints and execution require separate verification.
module capture_platform_top #(
 parameter integer CLOCK_COMMAND_TIMEOUT_CYCLES=250000000,CLK_HZ=50000000,FFT_CLOCK_GATE=0,DEBUG_INGEST_STALL=0,
 FIFO_DEPTH=1024,RESET_HOLD_CYCLES=8,ADC_WARMUP_CYCLES=100000000,
 START_TIMEOUT_CYCLES=250000000,DRAIN_TIMEOUT_CYCLES=5000000,
 BCK_TIMEOUT_CYCLES=50000,SLOT_TIMEOUT_CYCLES=500000,TEST_TIMEOUT_CYCLES=500000,
 parameter SINE_FILE="generated/pattern_sine.mem",
 parameter FIR0_FILE="generated/fir_stage1.mem",FIR1_FILE="generated/fir_stage2.mem",
 parameter FIR2_FILE="generated/fir_stage3.mem",HANN_FILE="generated/hann4096.mem",
 parameter integer CONTROL_TIMEOUT_CYCLES=250000000,OWNER_TIMEOUT_CYCLES=5000000,
 parameter[63:0]BOOT_ORIGIN=0
)(
(* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 clk CLK", X_INTERFACE_PARAMETER="ASSOCIATED_BUSIF s_cap_axi:s_dat_axi:s_ctr_axi:m_spectrum_axis:m_pcm_axis, ASSOCIATED_RESET aresetn, FREQ_HZ 50000000" *)
 input wire clk,
(* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 aresetn RST", X_INTERFACE_PARAMETER="POLARITY ACTIVE_LOW" *)
 input wire aresetn,
 input wire ref_clk_in,bclk_in,lrck_in,sdata_in,
 output wire mclk_out,
 input wire[11:0] s_cap_axi_awaddr,
 input wire s_cap_axi_awvalid,
 output wire s_cap_axi_awready,
 input wire[31:0] s_cap_axi_wdata,
 input wire[3:0] s_cap_axi_wstrb,
 input wire s_cap_axi_wvalid,
 output wire s_cap_axi_wready,
 output wire[1:0] s_cap_axi_bresp,
 output wire s_cap_axi_bvalid,
 input wire s_cap_axi_bready,
 input wire[11:0] s_cap_axi_araddr,
 input wire s_cap_axi_arvalid,
 output wire s_cap_axi_arready,
 output wire[31:0] s_cap_axi_rdata,
 output wire[1:0] s_cap_axi_rresp,
 output wire s_cap_axi_rvalid,
 input wire s_cap_axi_rready,
 input wire[11:0] s_dat_axi_awaddr,
 input wire s_dat_axi_awvalid,
 output wire s_dat_axi_awready,
 input wire[31:0] s_dat_axi_wdata,
 input wire[3:0] s_dat_axi_wstrb,
 input wire s_dat_axi_wvalid,
 output wire s_dat_axi_wready,
 output wire[1:0] s_dat_axi_bresp,
 output wire s_dat_axi_bvalid,
 input wire s_dat_axi_bready,
 input wire[11:0] s_dat_axi_araddr,
 input wire s_dat_axi_arvalid,
 output wire s_dat_axi_arready,
 output wire[31:0] s_dat_axi_rdata,
 output wire[1:0] s_dat_axi_rresp,
 output wire s_dat_axi_rvalid,
 input wire s_dat_axi_rready,
 input wire[11:0] s_ctr_axi_awaddr,
 input wire s_ctr_axi_awvalid,
 output wire s_ctr_axi_awready,
 input wire[31:0] s_ctr_axi_wdata,
 input wire[3:0] s_ctr_axi_wstrb,
 input wire s_ctr_axi_wvalid,
 output wire s_ctr_axi_wready,
 output wire[1:0] s_ctr_axi_bresp,
 output wire s_ctr_axi_bvalid,
 input wire s_ctr_axi_bready,
 input wire[11:0] s_ctr_axi_araddr,
 input wire s_ctr_axi_arvalid,
 output wire s_ctr_axi_arready,
 output wire[31:0] s_ctr_axi_rdata,
 output wire[1:0] s_ctr_axi_rresp,
 output wire s_ctr_axi_rvalid,
 input wire s_ctr_axi_rready,
 output wire m_spectrum_axis_tvalid,
 input wire m_spectrum_axis_tready,
 output wire[63:0] m_spectrum_axis_tdata,
 output wire[7:0] m_spectrum_axis_tkeep,
 output wire m_spectrum_axis_tlast,
 output wire m_pcm_axis_tvalid,
 input wire m_pcm_axis_tready,
 output wire[63:0] m_pcm_axis_tdata,
 output wire[7:0] m_pcm_axis_tkeep,
 output wire m_pcm_axis_tlast
);
 wire stop_complete;
 wire start_ready;
 wire running;
 wire command_rejected;
 wire[3:0] frontend_state;
 wire[3:0] fault_reason;
 wire[63:0] active_capture_id;
 wire[31:0] active_config_id;
 wire active_test_mode;
 wire mode_ready;
 wire mode_rejected;
 wire[1:0] mode_active;
 wire[63:0] adc_count;
 wire[63:0] test_count;
 wire[63:0] latest_raw;
 wire source_position_valid;
 wire mode_valid;
 wire[1:0] mode_select;
 wire start_req;
 wire stop_req;
 wire requested_test_mode;
 wire channel_select;
 wire[1:0] pattern_id;
 wire[31:0] requested_config_id;
 wire[63:0] next_capture_id;
 wire[63:0] origin_raw;
 wire armed;
 wire control_busy;
 wire spectrum_output_hold,pcm_output_hold,owner_busy;
 wire clock_command_req;
 wire[1:0] clock_command_kind;
 wire[31:0] clock_command_seq;
 wire clock_command_ack;
 wire clock_command_error;
 wire[31:0] clock_command_ack_seq;
 wire[3:0] clock_command_reason;
 wire clock_control_busy;
 wire audio_mclk_ready,audio_clock_fault,audio_restart_pending,audio_restart_clocks,audio_clear_clock_fault;
 wire capture_start;
 wire source_stopped;
 wire datapath_drained;
 wire[31:0] energy_occupancy;
 wire energy_valid;
 wire energy_ready;
 wire[63:0] energy_seq;
 wire[63:0] energy_capture;
 wire[63:0] energy_first_raw;
 wire[63:0] energy_value;
 wire[31:0] energy_config;
 wire[7:0] energy_flags;
 wire dma_idle;
 wire spectrum_flush_req;
 wire dma_spectrum_reset_done;
 wire dma_pcm_abort;
 wire clear_energy_records;
 wire[31:0] dma_spectrum_reset_seq;
 wire spectrum_flush_pending;
 wire[31:0] spectrum_flush_request_seq;
 wire[63:0] frame_candidates;
 wire[63:0] pcm_candidates;
 wire[63:0] raw_candidates;
 wire[63:0] energy_candidates;
 wire[3:0] grid_meta_valid;
 wire[255:0] grid_meta_capture;
 wire[255:0] grid_meta_seq;
 wire[255:0] grid_meta_raw;
 wire[127:0] grid_meta_config;
 wire spectrum_i_tvalid;
 wire[63:0] spectrum_i_tdata;
 wire[7:0] spectrum_i_tkeep;
 wire spectrum_i_tlast;
 wire pcm_i_tvalid;
 wire[63:0] pcm_i_tdata;
 wire[7:0] pcm_i_tkeep;
 wire pcm_i_tlast;
 assign capture_start=start_req&&!stop_req&&start_ready;
 assign source_stopped=frontend_state==0||frontend_state==6||frontend_state==7;
 assign m_spectrum_axis_tvalid=spectrum_i_tvalid&&!spectrum_output_hold;
 assign m_spectrum_axis_tdata=spectrum_i_tdata;
 assign m_spectrum_axis_tkeep=spectrum_i_tkeep;
 assign m_spectrum_axis_tlast=spectrum_i_tlast;
 assign m_pcm_axis_tvalid=pcm_i_tvalid&&!pcm_output_hold;
 assign m_pcm_axis_tdata=pcm_i_tdata;
 assign m_pcm_axis_tkeep=pcm_i_tkeep;
 assign m_pcm_axis_tlast=pcm_i_tlast;
 capture_control_regs #(.TIMEOUT_CYCLES(CONTROL_TIMEOUT_CYCLES),.BOOT_ORIGIN(BOOT_ORIGIN)) control(
  .clk(clk),
  .aresetn(aresetn),
  .awaddr(s_cap_axi_awaddr),
  .awvalid(s_cap_axi_awvalid),
  .awready(s_cap_axi_awready),
  .wdata(s_cap_axi_wdata),
  .wstrb(s_cap_axi_wstrb),
  .wvalid(s_cap_axi_wvalid),
  .wready(s_cap_axi_wready),
  .bresp(s_cap_axi_bresp),
  .bvalid(s_cap_axi_bvalid),
  .bready(s_cap_axi_bready),
  .araddr(s_cap_axi_araddr),
  .arvalid(s_cap_axi_arvalid),
  .arready(s_cap_axi_arready),
  .rdata(s_cap_axi_rdata),
  .rresp(s_cap_axi_rresp),
  .rvalid(s_cap_axi_rvalid),
  .rready(s_cap_axi_rready),
  .stop_complete(stop_complete),
  .start_ready(start_ready),
  .running(running),
  .command_rejected(command_rejected),
  .frontend_state(frontend_state),
  .fault_reason(fault_reason),
  .active_capture_id(active_capture_id),
  .active_config_id(active_config_id),
  .active_test_mode(active_test_mode),
  .mode_ready(mode_ready),
  .mode_rejected(mode_rejected),
  .mode_active(mode_active),
  .adc_count(adc_count),
  .test_count(test_count),
  .latest_raw(latest_raw),
  .source_position_valid(source_position_valid),
  .mode_valid(mode_valid),
  .mode_select(mode_select),
  .start_req(start_req),
  .stop_req(stop_req),
  .requested_test_mode(requested_test_mode),
  .channel_select(channel_select),
  .pattern_id(pattern_id),
  .requested_config_id(requested_config_id),
  .next_capture_id(next_capture_id),
  .origin_raw(origin_raw),
  .armed(armed),
  .service_busy(),
  .quarantined(),
  .transaction_busy(control_busy),
  .clock_command_req(clock_command_req),
  .clock_command_kind(clock_command_kind),
  .clock_command_seq(clock_command_seq),
  .clock_command_ack(clock_command_ack),
  .clock_command_error(clock_command_error),
  .clock_command_ack_seq(clock_command_ack_seq),
  .clock_command_reason(clock_command_reason),
  .mclk_ready_proc(audio_mclk_ready),.clock_fault_latched(audio_clock_fault),.clock_restart_pending(audio_restart_pending),
  .clock_control_busy(clock_control_busy)
 );
 capture_data_regs #(.TIMEOUT_CYCLES(OWNER_TIMEOUT_CYCLES)) ownership(
  .clk(clk),
  .aresetn(aresetn),
  .awaddr(s_dat_axi_awaddr),
  .awvalid(s_dat_axi_awvalid),
  .awready(s_dat_axi_awready),
  .wdata(s_dat_axi_wdata),
  .wstrb(s_dat_axi_wstrb),
  .wvalid(s_dat_axi_wvalid),
  .wready(s_dat_axi_wready),
  .bresp(s_dat_axi_bresp),
  .bvalid(s_dat_axi_bvalid),
  .bready(s_dat_axi_bready),
  .araddr(s_dat_axi_araddr),
  .arvalid(s_dat_axi_arvalid),
  .arready(s_dat_axi_arready),
  .rdata(s_dat_axi_rdata),
  .rresp(s_dat_axi_rresp),
  .rvalid(s_dat_axi_rvalid),
  .rready(s_dat_axi_rready),
  .capture_start(capture_start),
  .source_stopped(source_stopped),
  .drained(datapath_drained),
  .energy_occupancy(energy_occupancy),
  .energy_valid(energy_valid),
  .energy_ready(energy_ready),
  .energy_seq(energy_seq),
  .energy_capture(energy_capture),
  .energy_first_raw(energy_first_raw),
  .energy_value(energy_value),
  .energy_config(energy_config),
  .energy_flags(energy_flags),
  .busy(owner_busy),
  .quarantined(),
  .dma_idle(dma_idle),
  .spectrum_output_hold(spectrum_output_hold),
  .pcm_output_hold(pcm_output_hold),
  .spectrum_flush_req(spectrum_flush_req),
  .dma_spectrum_reset_done(dma_spectrum_reset_done),
  .dma_pcm_abort(dma_pcm_abort),
  .clear_energy_records(clear_energy_records),
  .dma_spectrum_reset_seq(dma_spectrum_reset_seq),
  .spectrum_flush_pending(spectrum_flush_pending),
  .spectrum_flush_request_seq(spectrum_flush_request_seq)
 );
 capture_contract_regs contract_bank(
  .clk(clk),
  .aresetn(aresetn),
  .awaddr(s_ctr_axi_awaddr),
  .awvalid(s_ctr_axi_awvalid),
  .awready(s_ctr_axi_awready),
  .wdata(s_ctr_axi_wdata),
  .wstrb(s_ctr_axi_wstrb),
  .wvalid(s_ctr_axi_wvalid),
  .wready(s_ctr_axi_wready),
  .bresp(s_ctr_axi_bresp),
  .bvalid(s_ctr_axi_bvalid),
  .bready(s_ctr_axi_bready),
  .araddr(s_ctr_axi_araddr),
  .arvalid(s_ctr_axi_arvalid),
  .arready(s_ctr_axi_arready),
  .rdata(s_ctr_axi_rdata),
  .rresp(s_ctr_axi_rresp),
  .rvalid(s_ctr_axi_rvalid),
  .rready(s_ctr_axi_rready),
  .armed(armed),
  .stop_complete(stop_complete),
  .control_busy(control_busy),
  .capture_start(capture_start),
  .next_capture_id(next_capture_id),
  .active_capture_id(active_capture_id),
  .requested_config_id(requested_config_id),
  .active_config_id(active_config_id),
  .requested_test_mode(requested_test_mode),
  .active_test_mode(active_test_mode),
  .channel_select(channel_select),
  .pattern_id(pattern_id),
  .mode_active(mode_active),
  .frame_candidates(frame_candidates),
  .pcm_candidates(pcm_candidates),
  .raw_candidates(raw_candidates),
  .energy_candidates(energy_candidates),
  .grid_meta_valid(grid_meta_valid),
  .grid_meta_capture(grid_meta_capture),
  .grid_meta_seq(grid_meta_seq),
  .grid_meta_raw(grid_meta_raw),
  .grid_meta_config(grid_meta_config)
 );
 audio_capture_top #(.CLOCK_COMMAND_TIMEOUT_CYCLES(CLOCK_COMMAND_TIMEOUT_CYCLES),.CLK_HZ(CLK_HZ),.FFT_CLOCK_GATE(FFT_CLOCK_GATE),.DEBUG_INGEST_STALL(DEBUG_INGEST_STALL),.FIFO_DEPTH(FIFO_DEPTH),.RESET_HOLD_CYCLES(RESET_HOLD_CYCLES),.ADC_WARMUP_CYCLES(ADC_WARMUP_CYCLES),.START_TIMEOUT_CYCLES(START_TIMEOUT_CYCLES),.DRAIN_TIMEOUT_CYCLES(DRAIN_TIMEOUT_CYCLES),.BCK_TIMEOUT_CYCLES(BCK_TIMEOUT_CYCLES),.SLOT_TIMEOUT_CYCLES(SLOT_TIMEOUT_CYCLES),.TEST_TIMEOUT_CYCLES(TEST_TIMEOUT_CYCLES),.SINE_FILE(SINE_FILE),.FIR0_FILE(FIR0_FILE),.FIR1_FILE(FIR1_FILE),.FIR2_FILE(FIR2_FILE),.HANN_FILE(HANN_FILE)) audio(
  .ref_clk_in(ref_clk_in),
  .bclk_in(bclk_in),
  .lrck_in(lrck_in),
  .sdata_in(sdata_in),
  .mclk_out(mclk_out),
  .proc_clk(clk),
  .rst(!aresetn),
  .start_req(start_req),
  .stop_req(stop_req),
  .requested_test_mode(requested_test_mode),
  .clock_command_req(clock_command_req),
  .clock_command_kind(clock_command_kind),
  .clock_command_seq(clock_command_seq),
  .clock_command_ack(clock_command_ack),
  .clock_command_error(clock_command_error),
  .mclk_ready_proc(audio_mclk_ready),.clock_fault_latched(audio_clock_fault),.clock_restart_pending(audio_restart_pending),
  .clock_control_busy(clock_control_busy),
  .clock_command_ack_seq(clock_command_ack_seq),
  .clock_command_reason(clock_command_reason),
  .restart_clocks(audio_restart_clocks),
  .clear_clock_fault(audio_clear_clock_fault),
  .active_test_mode(active_test_mode),
  .clock_snapshot_req(1'b0),
  .clock_snapshot_request_seq(32'd0),
  .clock_snapshot_ack(),
  .clock_snapshot_error(),
  .clock_snapshot_valid(),
  .clock_snapshot_ack_seq(),
  .clock_snapshot_data(),
  .clock_snapshot_age(),
  .next_capture_id(next_capture_id),
  .origin_raw(origin_raw),
  .requested_config_id(requested_config_id),
  .channel_select(channel_select),
  .pattern_id(pattern_id),
  .dma_idle(dma_idle),
  .dma_abort(1'b0),
  .start_ready(start_ready),
  .stop_complete(stop_complete),
  .running(running),
  .frontend_busy(),
  .command_rejected(command_rejected),
  .frontend_state(frontend_state),
  .fault_reason(fault_reason),
  .active_capture_id(active_capture_id),
  .fault_count(),
  .active_config_id(active_config_id),
  .adc_count(adc_count),
  .good_slot_count(),
  .test_count(test_count),
  .history_clear(),
  .cleanup_pending(),
  .clear_energy_records(clear_energy_records),
  .test_fft_input_stall(1'b0),
  .dma_spectrum_abort(1'b0),
  .dma_pcm_abort(dma_pcm_abort),
  .mode_valid(mode_valid),
  .mode_select(mode_select),
  .mode_ready(mode_ready),
  .mode_rejected(mode_rejected),
  .mode_active(mode_active),
  .spectrum_flush_req(spectrum_flush_req),
  .dma_spectrum_reset_done(dma_spectrum_reset_done),
  .dma_spectrum_reset_seq(dma_spectrum_reset_seq),
  .spectrum_flush_pending(spectrum_flush_pending),
  .spectrum_flush_done(),
  .spectrum_flush_timeout(),
  .spectrum_flush_request_seq(spectrum_flush_request_seq),
  .grid_meta_valid(grid_meta_valid),
  .grid_meta_capture(grid_meta_capture),
  .grid_meta_seq(grid_meta_seq),
  .grid_meta_raw(grid_meta_raw),
  .grid_meta_config(grid_meta_config),
  .processing_busy(),
  .drained(datapath_drained),
  .history_quiescent(),
  .fork_fft_drops(),
  .fork_pcm_drops(),
  .fork_energy_drops(),
  .fork_fft_highwater(),
  .fork_pcm_highwater(),
  .fork_energy_highwater(),
  .frame_candidates(frame_candidates),
  .frame_drops(),
  .frame_rejected(),
  .window_saturations(),
  .store_completed(),
  .store_drops(),
  .store_invalid(),
  .fft_packet_errors(),
  .fft_packets(),
  .pcm_invalid(),
  .pcm_packet_errors(),
  .pcm_packets(),
  .energy_discontinuities(),
  .energy_config_errors(),
  .energy_highwater(),
  .energy_occupancy(energy_occupancy),
  .energy_flush_count(),
  .pcm_flush_count(),
  .analysis_ready(),
  .source_position_valid(source_position_valid),
  .latest_raw(latest_raw),
  .fir0_saturations(),
  .fir1_saturations(),
  .fir2_saturations(),
  .raw_candidates(raw_candidates),
  .raw_completed(),
  .raw_drops(),
  .raw_invalid(),
  .fft_axis_tvalid(spectrum_i_tvalid),
  .fft_axis_tready(m_spectrum_axis_tready&&!spectrum_output_hold),
  .fft_axis_tdata(spectrum_i_tdata),
  .fft_axis_tkeep(spectrum_i_tkeep),
  .fft_axis_tlast(spectrum_i_tlast),
  .pcm_axis_tvalid(pcm_i_tvalid),
  .pcm_axis_tready(m_pcm_axis_tready&&!pcm_output_hold),
  .pcm_axis_tdata(pcm_i_tdata),
  .pcm_axis_tkeep(pcm_i_tkeep),
  .pcm_axis_tlast(pcm_i_tlast),
  .energy_valid(energy_valid),
  .energy_ready(energy_ready),
  .energy_seq(energy_seq),
  .energy_capture(energy_capture),
  .energy_first_raw(energy_first_raw),
  .energy_value(energy_value),
  .energy_config(energy_config),
  .energy_flags(energy_flags),
  .decimation_saturations(),
  .fft_completed(),
  .fft_drops(),
  .fft_timeouts(),
  .fft_errors(),
  .pcm_candidates(pcm_candidates),
  .pcm_drops(),
  .energy_candidates(energy_candidates),
  .energy_drops(),
  .last_ingest_cycles(),
  .last_job_cycles()
 );
endmodule
`default_nettype wire
