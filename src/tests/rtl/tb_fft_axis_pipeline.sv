// Actual generated AMD FFT, windowing and packet data are checked by the host.
// Fault controls gate both FFT input valid and ready to avoid phantom acceptance.
`timescale 1ns/1ps
module tb_fft_axis_pipeline;
 reg clk=0;always #10 clk=~clk;
 reg rst=1,abort=0,frame_reset=0,input_valid=0;
 reg signed[23:0]input_data=0;reg[63:0]capture=1,raw_index=0;
 reg[31:0]config_id=1;reg[7:0]flags=1;
 wire input_ready,w_valid,w_ready,w_last,frame_abort;
 wire signed[23:0]w_data;wire[63:0]w_capture,w_first,w_seq;
 wire[31:0]w_config;wire[7:0]w_flags;wire[11:0]w_index;
 wire[63:0]candidates,window_drops,window_sat,window_rejected;
 wire poisoned;wire[3:0]input_fault;
 frame_window window(.clk(clk),.rst(rst),.frame_reset(frame_reset),
  .s_valid(input_valid),.s_ready(input_ready),.s_data(input_data),
  .s_capture_id(capture),.s_raw_index(raw_index),.s_config_id(config_id),.s_flags(flags),
  .m_valid(w_valid),.m_ready(w_ready),.m_data(w_data),.m_capture_id(w_capture),
  .m_first_raw(w_first),.m_frame_seq(w_seq),.m_config_id(w_config),.m_flags(w_flags),
  .m_index(w_index),.m_last(w_last),.frame_abort(frame_abort),
  .candidate_count(candidates),.drop_count(window_drops),.saturation_count(window_sat),
  .rejected_count(window_rejected),.capture_poisoned(poisoned),.input_fault(input_fault));
 wire[7:0]cfg_data,status_data;wire cfg_valid,cfg_ready,status_valid,status_ready;
 wire[47:0]fft_in,fft_out;wire[23:0]fft_user;
 wire fft_iv,fft_ir,fft_il,fft_ov,fft_or,fft_ol,fft_resetn;
 wire missing,unexpected,in_halt,out_halt,status_halt;
 reg inject_stall=0;
 wire core_iv=fft_iv&&!inject_stall;
 wire core_ir;
 assign fft_ir=core_ir&&!inject_stall;
 lfn_fft core(.aclk(clk),.aresetn(fft_resetn),
  .s_axis_config_tdata(cfg_data),.s_axis_config_tvalid(cfg_valid),.s_axis_config_tready(cfg_ready),
  .s_axis_data_tdata(fft_in),.s_axis_data_tvalid(core_iv),.s_axis_data_tready(core_ir),.s_axis_data_tlast(fft_il),
  .m_axis_data_tdata(fft_out),.m_axis_data_tuser(fft_user),.m_axis_data_tvalid(fft_ov),.m_axis_data_tready(fft_or),.m_axis_data_tlast(fft_ol),
  .m_axis_status_tdata(status_data),.m_axis_status_tvalid(status_valid),.m_axis_status_tready(status_ready),
  .event_frame_started(),.event_tlast_missing(missing),.event_tlast_unexpected(unexpected),
  .event_data_in_channel_halt(in_halt),.event_data_out_channel_halt(out_halt),.event_status_channel_halt(status_halt));
 wire axis_valid,axis_last;wire[63:0]axis_data;wire[7:0]axis_keep;
 reg axis_ready=1;
 wire[63:0]completed,drops,timeouts,errors,ih,oh,sh,store_completed,store_drops,store_invalid,pack_completed,pack_errors;
 wire[31:0]ingest_cycles,job_cycles;
 fft_axis_pipeline #(.INGEST_CYCLES(49990)) dut(.clk(clk),.rst(rst),.abort(abort),.frame_abort(frame_abort),.packet_abort(1'b0),
  .s_valid(w_valid),.s_ready(w_ready),.s_data(w_data),.s_capture_id(w_capture),.s_first_raw(w_first),.s_frame_seq(w_seq),.s_config_id(w_config),.s_flags(w_flags),.s_index(w_index),.s_last(w_last),
  .fft_s_axis_config_tdata(cfg_data),.fft_s_axis_config_tvalid(cfg_valid),.fft_s_axis_config_tready(cfg_ready),
  .fft_s_axis_data_tdata(fft_in),.fft_s_axis_data_tvalid(fft_iv),.fft_s_axis_data_tready(fft_ir),.fft_s_axis_data_tlast(fft_il),
  .fft_m_axis_data_tdata(fft_out),.fft_m_axis_data_tuser(fft_user),.fft_m_axis_data_tvalid(fft_ov),.fft_m_axis_data_tready(fft_or),.fft_m_axis_data_tlast(fft_ol),
  .fft_m_axis_status_tdata(status_data),.fft_m_axis_status_tvalid(status_valid),.fft_m_axis_status_tready(status_ready),.fft_aresetn(fft_resetn),
  .event_tlast_missing(missing),.event_tlast_unexpected(unexpected),.event_data_in_channel_halt(in_halt),.event_data_out_channel_halt(out_halt),.event_status_channel_halt(status_halt),
  .m_axis_tvalid(axis_valid),.m_axis_tready(axis_ready),.m_axis_tdata(axis_data),.m_axis_tkeep(axis_keep),.m_axis_tlast(axis_last),
  .completed_count(completed),.drop_count(drops),.timeout_count(timeouts),.protocol_error_count(errors),.input_halt_count(ih),.output_halt_count(oh),.status_halt_count(sh),
  .last_ingest_cycles(ingest_cycles),.last_job_cycles(job_cycles),.store_completed_count(store_completed),.store_drop_count(store_drops),.store_invalid_count(store_invalid),.packet_completed_count(pack_completed),.packet_error_count(pack_errors));
 integer cycle=0,packet_count=0,word_count=0,packet_file,timing_file,window_file;
 integer physical_ready_cycle=0,first_offer_cycle=0,last_input_cycle=0;
 reg[63:0]held_data;reg held_last,was_stalled=0;
 always@(posedge clk)begin
  cycle<=cycle+1;
  if(cycle%50000==0)$display("TRACE fft_cycle=%0d state=%0d packets=%0d drops=%0d errors=%0d candidates=%0d rejected=%0d",cycle,dut.scheduler.state,packet_count,drops,errors,candidates,window_rejected);
  if(!rst&&dut.scheduler.fail_now)$display("TRACE fault cycle=%0d state=%0d deadline=%b input_bad=%b output_bad=%b status_bad=%b collect_error=%b status=%b/%h user=%h events=%b%b",cycle,dut.scheduler.state,dut.scheduler.deadline,dut.scheduler.input_bad,dut.scheduler.output_bad,dut.scheduler.status_bad,dut.scheduler.collect_error,status_valid,status_data,fft_user,missing,unexpected);
  if(!rst&&drops!=0&&packet_count==0)$fatal(1,"first actual FFT job failed drops=%0d errors=%0d timeouts=%0d",drops,errors,timeouts);
  if(!rst)begin
   if(!fft_resetn)first_offer_cycle=0;
   if(w_valid&&w_index==0&&first_offer_cycle==0)first_offer_cycle=cycle;
   if(fft_iv&&fft_ir&&fft_il)begin
    last_input_cycle=cycle;
    if(cycle-physical_ready_cycle>=50000)$fatal(1,"physical input bank release deadline");
   end
   if(w_valid&&w_ready)$fdisplay(window_file,"%0d %0d %06x",w_seq,w_index,w_data);
   if(was_stalled&&(!axis_valid||axis_data!==held_data||axis_last!==held_last))$fatal(1,"unstable AXIS offer");
   was_stalled=axis_valid&&!axis_ready;held_data=axis_data;held_last=axis_last;
   if(axis_valid&&axis_ready)begin
    if(axis_keep!=8'hff)$fatal(1,"bad AXIS keep");
    $fdisplay(packet_file,"%016x %0d",axis_data,axis_last);
    word_count<=word_count+1;
    if(axis_last)begin
     if(word_count!=4103)$fatal(1,"packet length %0d",word_count);
     $fdisplay(timing_file,"%0d %0d %0d %0d %0d %0d %0d",packet_count,physical_ready_cycle,first_offer_cycle,last_input_cycle,cycle,ingest_cycles,job_cycles);
     packet_count<=packet_count+1;word_count<=0;first_offer_cycle=0;
    end
   end
  end
 end
 reg signed[23:0]samples[0:12287];
 integer i,j,target;
 task feed(input integer start,input integer count);
  begin
   for(j=start;j<start+count;j=j+1)begin
    @(negedge clk);input_valid=1;input_data=samples[j];raw_index=j*48;
    @(posedge clk);if(!input_ready)$fatal(1,"physical input blocked");
   end
   @(negedge clk);input_valid=0;physical_ready_cycle=cycle-1;
  end
 endtask
 task await_packet(input integer wanted);
  integer guard;
  begin
   guard=0;
   while(packet_count<wanted&&guard<500000)begin
    @(negedge clk);axis_ready=((cycle%13)!=0)&&((cycle%17)!=0);guard=guard+1;
   end
   if(packet_count!=wanted)$fatal(1,"packet timeout wanted=%0d completed=%0d errors=%0d drops=%0d",wanted,completed,errors,drops);
   repeat(5)@(negedge clk);
  end
 endtask
 initial begin
  packet_file=$fopen("fft_packets.txt","w");timing_file=$fopen("fft_timing.txt","w");window_file=$fopen("fft_window.txt","w");
  if(!packet_file||!timing_file||!window_file)$fatal(1,"output open failed");
  $readmemh("fft_samples.mem",samples);
  repeat(5)@(negedge clk);rst=0;
  feed(0,4096);await_packet(1);
  feed(4096,2048);await_packet(2);
  feed(6144,2048);await_packet(3);
  // Partly load frame4, then starve input. Gate both sides of core handshake.
  feed(8192,2048);
  wait(fft_iv&&fft_ir&&w_index==100);@(negedge clk);inject_stall=1;
  wait(timeouts==1);wait(fft_resetn==0);wait(fft_resetn==1);
  @(negedge clk);inject_stall=0;
  // Let scheduler drain the failed source only, preserving frame-grid history.
  repeat(5000)@(negedge clk);
  if(packet_count!=3||drops!=1||errors!=0)$fatal(1,"partial recovery accounting");
  feed(10240,2048);await_packet(4);
  if(completed!=4||store_completed!=4||pack_completed!=4||store_drops!=1||store_invalid!=0||pack_errors!=0||timeouts!=1||errors!=0)$fatal(1,"final counters");
  if(window_drops!=0||window_rejected!=0||window_sat!=0)$fatal(1,"window counters");
  $fclose(packet_file);$fclose(timing_file);$fclose(window_file);
  $display("PASS fft_axis_pipeline");$finish;
 end
 initial begin #100000000;$fatal(1,"global watchdog");end
endmodule
