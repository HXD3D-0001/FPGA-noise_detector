// Source-independent processing in proc_clk domain (clk).
// Upstream owns capture validation, continuity, source gating and recovery.
// Hold all s_* payload fields stable while s_valid && !s_ready. Quiesce source
// before draining; clear_history cancels partial analysis, never DMA ownership.
// start clears position/readiness only; it does not create or reset a source.
module sample_processing_core #(
 parameter integer CLK_HZ=50000000,FFT_CLOCK_GATE=0,DEBUG_INGEST_STALL=0,
 parameter FIR0_FILE="generated/fir_stage1.mem",FIR1_FILE="generated/fir_stage2.mem",
 parameter FIR2_FILE="generated/fir_stage3.mem",HANN_FILE="generated/hann4096.mem"
)(input wire clk,rst,start,clear_history,dma_abort,
 input wire s_valid,output wire s_ready,input wire signed[23:0]s_data,
 input wire[63:0]s_capture_id,s_raw_index,input wire[7:0]s_flags,
 input wire clear_energy_records,
 input wire test_fft_input_stall,
 input wire dma_spectrum_abort,dma_pcm_abort,
 input wire mode_valid,stopped,input wire[1:0]mode_select,
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
 output reg analysis_ready,source_position_valid,
 output reg[63:0]latest_raw,
 output wire[63:0]fir0_saturations,fir1_saturations,fir2_saturations,
 output wire[63:0]raw_candidates,raw_completed,raw_drops,raw_invalid,
 input wire[31:0]config_id,
 output wire fft_axis_tvalid,input wire fft_axis_tready,output wire[63:0]fft_axis_tdata,
 output wire[7:0]fft_axis_tkeep,output wire fft_axis_tlast,
 output wire pcm_axis_tvalid,input wire pcm_axis_tready,output wire[63:0]pcm_axis_tdata,
 output wire[7:0]pcm_axis_tkeep,output wire pcm_axis_tlast,
 output wire energy_valid,input wire energy_ready,output wire[63:0]energy_seq,energy_capture,energy_first_raw,energy_value,
 output wire[31:0]energy_config,output wire[7:0]energy_flags,
 output wire[63:0]decimation_saturations,
 output wire[63:0]fft_completed,fft_drops,fft_timeouts,fft_errors,pcm_candidates,pcm_drops,energy_candidates,energy_drops,
 output wire[31:0]last_ingest_cycles,last_job_cycles);
 // The physical input bank must be FREE before the next 1kHz sample/hop.
 // On any ingest fault RESET is followed by up to 4096 reader DRAIN beats.
 // Reserve 32 more clocks for READY/prefetch, fault transition and release.
 // At 50MHz: 50000-4096-2-32 = 45870 ingest clocks (917.4us).
 // Low-rate accelerated RAW tests have no feasible FFT bank deadline; clamp
 // to a legal watchdog, without claiming FFT operation or nominal deadlines.
 localparam integer FFT_BANK_CYCLES=CLK_HZ/1000,FFT_DRAIN_CYCLES=4096;
 localparam integer FFT_RESET_CYCLES=2,FFT_RELEASE_MARGIN=32;
 localparam integer FFT_INGEST_CYCLES=FFT_BANK_CYCLES>FFT_DRAIN_CYCLES+FFT_RESET_CYCLES+FFT_RELEASE_MARGIN+2 ?
  FFT_BANK_CYCLES-FFT_DRAIN_CYCLES-FFT_RESET_CYCLES-FFT_RELEASE_MARGIN : 2;
 wire rv=s_valid;wire rr;assign s_ready=rr;
 wire signed[23:0]rd=s_data;wire[63:0]rc=s_capture_id,ri=s_raw_index;wire[7:0]rf=s_flags;
 wire dv,dr,decimation_ready,raw_ready;
 wire decimation_pending,window_pending,pcm_pending;
 wire monitoring=mode_active==0;
 assign rr=monitoring?decimation_ready:raw_ready;wire signed[23:0]dd;wire[63:0]dc,di;wire[7:0]df;
 decimator_chain #(.STAGE1_FILE(FIR0_FILE),.STAGE2_FILE(FIR1_FILE),.STAGE3_FILE(FIR2_FILE)) decimation(
  .clk(clk),.rst(rst),.clear(clear_history),.s_valid(rv&&monitoring),.s_ready(decimation_ready),.s_data(rd),.s_capture_id(rc),.s_raw_anchor(ri),.s_flags(rf),
  .m_valid(dv),.m_ready(dr),.m_data(dd),.m_capture_id(dc),.m_raw_anchor(di),.m_flags(df),.m_warmup(),.saturation_count(decimation_saturations),
  .stage1_saturation_count(fir0_saturations),.stage2_saturation_count(fir1_saturations),.stage3_saturation_count(fir2_saturations),.work_pending(decimation_pending));
 wire f0v,f0r,f1v,f1r,f2v,f2r;wire signed[23:0]f0d,f1d,f2d;
 wire[63:0]f0c,f0i,f1c,f1i,f2c,f2i;wire[7:0]f0f,f1f,f2f;
 sample_fork branches(.clk(clk),.rst(rst),.clear(clear_history),.s_valid(dv&&monitoring),.s_ready(dr),.s_data(dd),.s_capture_id(dc),.s_raw_index(di),.s_flags(df),
  .m0_valid(f0v),.m0_ready(f0r),.m0_data(f0d),.m0_capture_id(f0c),.m0_raw_index(f0i),.m0_flags(f0f),.m0_drop_count(fork_fft_drops),.m0_high_water(fork_fft_highwater),
  .m1_valid(f1v),.m1_ready(f1r),.m1_data(f1d),.m1_capture_id(f1c),.m1_raw_index(f1i),.m1_flags(f1f),.m1_drop_count(fork_pcm_drops),.m1_high_water(fork_pcm_highwater),
  .m2_valid(f2v),.m2_ready(f2r),.m2_data(f2d),.m2_capture_id(f2c),.m2_raw_index(f2i),.m2_flags(f2f),.m2_drop_count(fork_energy_drops),.m2_high_water(fork_energy_highwater));
 energy_meter #(.ENABLE_RECORD_FLUSH(1)) energy(.clk(clk),.grid_meta_valid(grid_meta_valid[3+:1]),.grid_meta_capture(grid_meta_capture[192+:64]),.grid_meta_config(grid_meta_config[96+:32]),.grid_meta_seq(grid_meta_seq[192+:64]),.grid_meta_raw(grid_meta_raw[192+:64]),.rst(rst),.clear(clear_history),.fifo_flush(clear_energy_records),.occupancy(energy_occupancy),.flush_count(energy_flush_count),.s_valid(f2v&&monitoring),.s_ready(f2r),.s_data(f2d),
  .s_capture_id(f2c),.s_raw_index(f2i),.s_config_id(config_id),.s_flags(f2f),
  .m_valid(energy_valid),.m_ready(energy_ready),.m_seq(energy_seq),.m_capture_id(energy_capture),.m_first_raw(energy_first_raw),
  .m_energy(energy_value),.m_config_id(energy_config),.m_flags(energy_flags),.candidate_count(energy_candidates),.drop_count(energy_drops),
  .discontinuity_count(energy_discontinuities),.config_error_count(energy_config_errors),.high_water(energy_highwater));
 wire pv,pr,pdone,pe,re,readvalid;wire[15:0]pt,pf;wire[63:0]pc,ps,pi;wire[31:0]pconfig,readword;wire[7:0]pexp;wire[12:0]ra;
 pcm_packet_store #(.ENABLE_BANK_FLUSH(1),.FIXED_CAPTURE_GRID(1)) pcm(.clk(clk),.grid_meta_valid(grid_meta_valid[1+:1]),.grid_meta_capture(grid_meta_capture[64+:64]),.grid_meta_config(grid_meta_config[32+:32]),.grid_meta_seq(grid_meta_seq[64+:64]),.grid_meta_raw(grid_meta_raw[64+:64]),.rst(rst),.bank_flush(dma_abort||dma_pcm_abort),.flush_count(pcm_flush_count),.analysis_reset(clear_history),.s_valid(f1v&&monitoring),.s_ready(f1r),.s_data(f1d),.s_capture_id(f1c),
  .s_raw_index(f1i),.s_config_id(config_id),.s_flags(f1f),.packet_valid(pv),.packet_ready(pr),.packet_type(pt),.packet_flags(pf),
  .packet_capture_id(pc),.packet_seq(ps),.packet_first_raw(pi),.packet_config_id(pconfig),.packet_blk_exp(pexp),
  .read_en(re),.read_addr(ra),.read_valid(readvalid),.read_data(readword),.packet_done(pdone),.candidate_count(pcm_candidates),.drop_count(pcm_drops),.invalid_count(pcm_invalid),.work_pending(pcm_pending));
 axis_packetizer pcm_wire(.clk(clk),.rst(rst),.abort(dma_abort||dma_pcm_abort),.packet_valid(pv),.packet_ready(pr),.packet_type(pt),.packet_flags(pf),
  .packet_capture_id(pc),.packet_seq(ps),.packet_first_raw(pi),.packet_config_id(pconfig),.packet_blk_exp(pexp),.packet_done(pdone),.packet_error(pe),
  .read_en(re),.read_addr(ra),.read_valid(readvalid),.read_data(readword),.m_axis_tvalid(pcm_axis_tvalid),.m_axis_tready(pcm_axis_tready),
  .m_axis_tdata(pcm_axis_tdata),.m_axis_tkeep(pcm_axis_tkeep),.m_axis_tlast(pcm_axis_tlast),.completed_count(pcm_packets),.error_count(pcm_packet_errors));
 wire wv,wr,wl,wa;wire signed[23:0]wd;wire[63:0]wc,wi,ws;wire[31:0]wconfig;wire[7:0]wf;wire[11:0]wx;
 frame_window #(.HANN_FILE(HANN_FILE),.FIXED_CAPTURE_GRID(1)) window(.clk(clk),.grid_meta_valid(grid_meta_valid[0+:1]),.grid_meta_capture(grid_meta_capture[0+:64]),.grid_meta_config(grid_meta_config[0+:32]),.grid_meta_seq(grid_meta_seq[0+:64]),.grid_meta_raw(grid_meta_raw[0+:64]),.rst(rst),.frame_reset(clear_history),.s_valid(f0v&&monitoring),.s_ready(f0r),.s_data(f0d),
  .s_capture_id(f0c),.s_raw_index(f0i),.s_config_id(config_id),.s_flags(f0f),.m_valid(wv),.m_ready(wr),.m_data(wd),
  .m_capture_id(wc),.m_first_raw(wi),.m_frame_seq(ws),.m_config_id(wconfig),.m_flags(wf),.m_index(wx),.m_last(wl),.frame_abort(wa),
  .candidate_count(frame_candidates),.drop_count(frame_drops),.saturation_count(window_saturations),.rejected_count(frame_rejected),.capture_poisoned(),.input_fault(),.work_pending(window_pending));
 wire[7:0]cfg_data,status_data;wire cfg_valid,cfg_ready,status_valid,status_ready;
 wire[47:0]fft_in,fft_out;wire[23:0]fft_user;wire iv,ir,il,ov,orr,ol,resetn;
 wire missing,unexpected,ih,oh,sh;
 wire core_clock_enable,core_clk,core_clock_active;
 wire input_stalled=DEBUG_INGEST_STALL&&test_fft_input_stall;
 fft_clock_gate #(.ENABLE(FFT_CLOCK_GATE)) clock_gate(.clk(clk),.rst(rst),.enable(core_clock_enable||ov||status_valid),.core_clk(core_clk),.core_active(core_clock_active));
 lfn_fft core(.aclk(core_clk),.aresetn(resetn),.s_axis_config_tdata(cfg_data),.s_axis_config_tvalid(cfg_valid&&core_clock_active),.s_axis_config_tready(cfg_ready),
  .s_axis_data_tdata(fft_in),.s_axis_data_tvalid(iv&&core_clock_active&&!input_stalled),.s_axis_data_tready(ir),.s_axis_data_tlast(il),
  .m_axis_data_tdata(fft_out),.m_axis_data_tuser(fft_user),.m_axis_data_tvalid(ov),.m_axis_data_tready(orr&&core_clock_active),.m_axis_data_tlast(ol),
  .m_axis_status_tdata(status_data),.m_axis_status_tvalid(status_valid),.m_axis_status_tready(status_ready&&core_clock_active),.event_frame_started(),
  .event_tlast_missing(missing),.event_tlast_unexpected(unexpected),.event_data_in_channel_halt(ih),.event_data_out_channel_halt(oh),.event_status_channel_halt(sh));
 wire fft_busy,store_idle,fft_analysis_pending;
 fft_axis_pipeline #(.INGEST_CYCLES(FFT_INGEST_CYCLES),.JOB_CYCLES(CLK_HZ/10),.RESET_CYCLES(FFT_RESET_CYCLES),.SHARED_STORE(1)) fft(
  .store_mode_valid(mode_valid),.store_mode_select(mode_select==3?2'd0:mode_select),.store_stopped(stopped),
  .store_mode_ready(mode_ready),.store_mode_rejected(mode_rejected),.store_mode_active(mode_active),
  .store_flush_req(spectrum_flush_req),.dma_spectrum_reset_done(dma_spectrum_reset_done),.dma_spectrum_reset_seq(dma_spectrum_reset_seq),
  .store_flush_pending(spectrum_flush_pending),.store_flush_done(spectrum_flush_done),.store_flush_timeout(spectrum_flush_timeout),.store_flush_request_seq(spectrum_flush_request_seq),
  .raw_valid(rv&&!monitoring),.raw_ready(raw_ready),.raw_data(rd),.raw_capture_id(rc),.raw_index(ri),.raw_config_id(config_id),.raw_flags(rf),
  .raw_grid_meta_valid(grid_meta_valid[2+:1]),.raw_grid_meta_capture(grid_meta_capture[128+:64]),.raw_grid_meta_config(grid_meta_config[64+:32]),.raw_grid_meta_seq(grid_meta_seq[128+:64]),.raw_grid_meta_raw(grid_meta_raw[128+:64]),.raw_candidate_count(raw_candidates),.raw_completed_count(raw_completed),.raw_drop_count(raw_drops),.raw_invalid_count(raw_invalid),
  .analysis_pending(fft_analysis_pending),.pipeline_busy(fft_busy),.store_idle(store_idle),.core_clock_enable(core_clock_enable),
  .clk(clk),.rst(rst),.abort(clear_history),.frame_abort(wa),.packet_abort(dma_abort||dma_spectrum_abort),.s_valid(wv&&monitoring),.s_ready(wr),.s_data(wd),
  .s_capture_id(wc),.s_first_raw(wi),.s_frame_seq(ws),.s_config_id(wconfig),.s_flags(wf),.s_index(wx),.s_last(wl),
  .fft_s_axis_config_tdata(cfg_data),.fft_s_axis_config_tvalid(cfg_valid),.fft_s_axis_config_tready(cfg_ready&&core_clock_active),
  .fft_s_axis_data_tdata(fft_in),.fft_s_axis_data_tvalid(iv),.fft_s_axis_data_tready(ir&&core_clock_active&&!input_stalled),.fft_s_axis_data_tlast(il),
  .fft_m_axis_data_tdata(fft_out),.fft_m_axis_data_tuser(fft_user),.fft_m_axis_data_tvalid(ov&&core_clock_active),.fft_m_axis_data_tready(orr),.fft_m_axis_data_tlast(ol),
  .fft_m_axis_status_tdata(status_data),.fft_m_axis_status_tvalid(status_valid&&core_clock_active),.fft_m_axis_status_tready(status_ready),.fft_aresetn(resetn),
  .event_tlast_missing(missing),.event_tlast_unexpected(unexpected),.event_data_in_channel_halt(ih),.event_data_out_channel_halt(oh),.event_status_channel_halt(sh),
  .completed_count(fft_completed),.drop_count(fft_drops),.timeout_count(fft_timeouts),.protocol_error_count(fft_errors),
  .input_halt_count(),.output_halt_count(),.status_halt_count(),.last_ingest_cycles(last_ingest_cycles),.last_job_cycles(last_job_cycles),
  .m_axis_tvalid(fft_axis_tvalid),.m_axis_tready(fft_axis_tready),.m_axis_tdata(fft_axis_tdata),.m_axis_tkeep(fft_axis_tkeep),.m_axis_tlast(fft_axis_tlast),
  .store_completed_count(store_completed),.store_drop_count(store_drops),.store_invalid_count(store_invalid),.packet_completed_count(fft_packets),.packet_error_count(fft_packet_errors));
 reg pcm_owned;
 always @(posedge clk)begin
  if(rst||start)begin analysis_ready<=0;source_position_valid<=0;latest_raw<=0;end
  else begin
   if(rv&&rr)begin source_position_valid<=1;latest_raw<=ri;end
   if(dv&&dr&&df[0]&&!df[1])analysis_ready<=1;
  end
  if(rst)pcm_owned<=0;
  else begin
   if(pv&&pr)pcm_owned<=1;
   if(pdone||dma_abort||dma_pcm_abort)pcm_owned<=0;
  end
 end
 // Quiesce source first, drain accepted beats/jobs/packets, then clear partial
 // histories. External DMA completion is an additional stop barrier.
 assign processing_busy=fft_busy||pcm_owned||pv||rv||dv||f0v||f1v||f2v||wv||
  decimation_pending||window_pending||pcm_pending;
 // Excludes incomplete FILL histories and published packets, both preserved
 // by their existing clear/ownership rules. It never excludes accepted MAC,
 // slow grid arithmetic, complete window readers, or a live FFT job.
 assign history_quiescent=!(rv||dv||f0v||f1v||f2v||wv||decimation_pending||window_pending||pcm_pending||fft_analysis_pending);
 assign drained=!processing_busy&&!spectrum_flush_pending;
endmodule

