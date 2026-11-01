`timescale 1ns/1ps
// Protocol mock only. Does not model FFT arithmetic, AMD timing, or storage RAM.
module tb_fft_scheduler;
 reg clk=0;always #5 clk=~clk;
 reg rst=1,abort=0,frame_abort=0;
 reg s_valid=0;wire s_ready;reg signed[23:0]s_data=0;
 reg[63:0]s_capture_id=1,s_first_raw=47,s_frame_seq=0;reg[31:0]s_config_id=1;
 reg[7:0]s_flags=1;reg[11:0]s_index=0;reg s_last=0;
 wire[7:0]fft_s_axis_config_tdata;wire fft_s_axis_config_tvalid;reg fft_s_axis_config_tready=1;
 wire[47:0]fft_s_axis_data_tdata;wire fft_s_axis_data_tvalid;reg fft_s_axis_data_tready=1;wire fft_s_axis_data_tlast;
 reg[47:0]fft_m_axis_data_tdata=0;reg[23:0]fft_m_axis_data_tuser=0;reg fft_m_axis_data_tvalid=0;wire fft_m_axis_data_tready;reg fft_m_axis_data_tlast=0;
 reg[7:0]fft_m_axis_status_tdata=0;reg fft_m_axis_status_tvalid=0;wire fft_m_axis_status_tready,fft_aresetn;
 reg event_tlast_missing=0,event_tlast_unexpected=0,event_data_in_channel_halt=0,event_data_out_channel_halt=0,event_status_channel_halt=0;
 wire reserve_valid;reg reserve_ready=1;wire[63:0]reserve_capture_id,reserve_first_raw,reserve_frame_seq;
 wire[31:0]reserve_config_id;wire[7:0]reserve_flags;wire store_commit,store_abort;
 wire result_valid;wire result_ready;wire signed[23:0]result_real,result_imag;
 wire[11:0]result_index;wire[7:0]result_blk_exp;wire result_last;
 reg collect_done=0,collect_error=0;reg[7:0]collect_blk_exp=0;
 wire[63:0]completed_count,drop_count,timeout_count,protocol_error_count,input_halt_count,output_halt_count,status_halt_count;
 wire[31:0]last_ingest_cycles,last_job_cycles;
 localparam INGEST=20000,JOB=50000;
 fft_scheduler #(.INGEST_CYCLES(INGEST),.JOB_CYCLES(JOB)) dut(.*);
 integer cycles=0,input_count=0,result_count=0,configs=0,commits=0,aborts=0,reservations=0,reset_run=0,resets=0,total_input_handshakes=0;
 reg configured=0,private_active=0,store_space=1,output_space=1,previous_resetn=0;
 reg config_stalled=0,input_stalled=0,result_stalled=0,reserve_stalled=0;
 reg[47:0]held_input;reg held_last;reg[68:0]held_result;reg[231:0]held_reserve;
 reg expect_gap=0;
 assign result_ready=private_active&&!collect_done&&output_space;
 always@(posedge clk)begin
  cycles=cycles+1;if(cycles>1000000)$fatal(1,"scheduler test timeout");
  if(!rst)begin
   if(!fft_aresetn)begin reset_run=reset_run+1;configured=0;input_count=0;end
   if(fft_aresetn&&!previous_resetn)begin if(reset_run<2)$fatal(1,"FFT reset shorter than two clocks");resets=resets+1;reset_run=0;end
   previous_resetn=fft_aresetn;
   if(config_stalled&&fft_aresetn&&(!fft_s_axis_config_tvalid||fft_s_axis_config_tdata!=1))$fatal(1,"configuration changed while stalled");
   config_stalled=fft_s_axis_config_tvalid&&!fft_s_axis_config_tready;
   if(input_stalled&&fft_aresetn&&(!fft_s_axis_data_tvalid||fft_s_axis_data_tdata!==held_input||fft_s_axis_data_tlast!==held_last))$fatal(1,"FFT input changed while stalled");
   input_stalled=fft_s_axis_data_tvalid&&!fft_s_axis_data_tready;held_input=fft_s_axis_data_tdata;held_last=fft_s_axis_data_tlast;
   if(reserve_stalled&&fft_aresetn&&(!reserve_valid||{reserve_capture_id,reserve_first_raw,reserve_frame_seq,reserve_config_id,reserve_flags}!==held_reserve))$fatal(1,"reservation changed while stalled");
   reserve_stalled=reserve_valid&&!reserve_ready;held_reserve={reserve_capture_id,reserve_first_raw,reserve_frame_seq,reserve_config_id,reserve_flags};
   if(result_stalled&&fft_aresetn&&(!result_valid||{result_real,result_imag,result_index,result_blk_exp,result_last}!==held_result))$fatal(1,"result changed while stalled");
   result_stalled=result_valid&&!result_ready;held_result={result_real,result_imag,result_index,result_blk_exp,result_last};
   if(fft_s_axis_config_tvalid&&fft_s_axis_config_tready)begin
    if(fft_s_axis_config_tdata!=1||!private_active)$fatal(1,"config direction or ordering");configured=1;configs=configs+1;input_count=0;
   end
   if(fft_s_axis_data_tvalid&&fft_s_axis_data_tready)begin
    if(!configured||!private_active||fft_s_axis_data_tdata[47:24]!=0||fft_s_axis_data_tlast!=(input_count==4095))$fatal(1,"FFT input contract");
    if(fft_s_axis_data_tdata[23:0]!==s_data)$fatal(1,"FFT real data mismatch");input_count=input_count+1;total_input_handshakes=total_input_handshakes+1;
   end
   collect_error<=0;
   if(reserve_valid&&reserve_ready)begin
    if(expect_gap&&!reserve_flags[2])$fatal(1,"failure GAP was not retained for next reservation");
    expect_gap=0;private_active=1;configured=0;input_count=0;result_count=0;collect_done<=0;reservations=reservations+1;
   end
   if(result_valid&&result_ready)begin
    if(result_index!=result_count||result_last!=(result_count==4095)||(result_count!=0&&result_blk_exp!=collect_blk_exp))collect_error<=1;
    collect_blk_exp<=result_blk_exp;result_count=result_count+1;
    if(result_count==4096)collect_done<=1;
   end
   if(store_commit)begin
    if(!private_active||!collect_done||result_count!=4096)$fatal(1,"committed incomplete private result");
    commits=commits+1;private_active=0;collect_done<=0;
   end
   if(store_abort)begin aborts=aborts+1;private_active=0;collect_done<=0;result_count=0;end
  end else begin
   configured=0;private_active=0;collect_done<=0;collect_error<=0;input_count=0;reset_run=reset_run+1;previous_resetn=0;
  end
 end
 task offer_word(input integer index,input integer frame);
  begin
   @(negedge clk);s_valid=1;s_index=index;s_last=index==4095;s_data=-index;
   s_capture_id=1;s_first_raw=47;s_frame_seq=frame;s_config_id=1;s_flags=1;
   do @(posedge clk);while(!s_ready);
   @(negedge clk);s_valid=0;
  end
 endtask
 task send_input(input integer frame);
  integer i;begin for(i=0;i<4096;i=i+1)offer_word(i,frame);end
 endtask
 task send_results(input integer exponent);
  integer i;begin
   wait(private_active&&configured&&input_count==4096);
   for(i=0;i<4096;i=i+1)begin
    @(negedge clk);fft_m_axis_data_tdata={24'(i),24'(-i)};
    fft_m_axis_data_tuser={8'(exponent),4'd0,12'(i)};fft_m_axis_data_tlast=i==4095;fft_m_axis_data_tvalid=1;
    do @(posedge clk);while(!fft_m_axis_data_tready);
    @(negedge clk);fft_m_axis_data_tvalid=0;
   end
  end
 endtask
 task send_status(input integer exponent);
  begin @(negedge clk);fft_m_axis_status_tdata=exponent;fft_m_axis_status_tvalid=1;
   @(negedge clk);fft_m_axis_status_tvalid=0;end
 endtask
 task full_job(input integer frame);
  integer expected;begin
   expected=completed_count+1;
   fork send_input(frame);begin send_results(3);repeat(8)@(negedge clk);send_status(3);end join
   repeat(6)@(negedge clk);
   if(completed_count!=expected||commits!=expected)$fatal(1,"normal FFT job not committed");
   if(last_ingest_cycles>=INGEST||last_job_cycles>=JOB)$fatal(1,"normal job exceeded budget");
  end
 endtask
 task pulse_abort(input integer upstream_only);
  begin @(negedge clk);if(upstream_only)frame_abort=1;else abort=1;
   @(negedge clk);frame_abort=0;abort=0;end
 endtask
 task assert_failure(input integer expected_drop,input integer expected_protocol,input integer expected_timeout);
  begin repeat(8)@(negedge clk);
   if(drop_count!=expected_drop||protocol_error_count!=expected_protocol||timeout_count!=expected_timeout||private_active)
    $fatal(1,"failure counters/private ownership got=%0d/%0d/%0d wanted=%0d/%0d/%0d",drop_count,protocol_error_count,timeout_count,expected_drop,expected_protocol,expected_timeout);
   expect_gap=1;
  end
 endtask
 task boundary_input(input integer frame,input integer finish_timer);
  integer i;begin
   for(i=0;i<4095;i=i+1)offer_word(i,frame);
   @(negedge clk);s_valid=1;s_index=4095;s_last=1;s_data=-4095;s_frame_seq=frame;fft_s_axis_data_tready=0;
   while(dut.ingest_timer<finish_timer)@(negedge clk);
   fft_s_axis_data_tready=1;
   do @(posedge clk);while(!s_ready);
   @(negedge clk);s_valid=0;
  end
 endtask
 integer d,p,t,c,i,before_inputs;
 initial begin
  repeat(5)@(negedge clk);rst=0;
  full_job(0);
  $display("EVIDENCE scheduler protocol mock normal ingest_cycles=%0d job_cycles=%0d",last_ingest_cycles,last_job_cycles);
  // Stalls preserve reservation, forward configuration, input, and result.
  reserve_ready=0;fft_s_axis_config_tready=0;output_space=0;
  fork full_job(1);begin
   wait(reserve_valid);repeat(17)@(negedge clk);reserve_ready=1;
   wait(fft_s_axis_config_tvalid);repeat(11)@(negedge clk);fft_s_axis_config_tready=1;
   wait(fft_m_axis_data_tvalid);repeat(19)@(negedge clk);output_space=1;
  end join
  d=drop_count;p=protocol_error_count;t=timeout_count;c=completed_count;
  // Halt diagnostics are not immediate faults.
  fork full_job(2);begin
   wait(input_count==12);@(negedge clk);event_data_in_channel_halt=1;event_data_out_channel_halt=1;event_status_channel_halt=1;
   @(negedge clk);event_data_in_channel_halt=0;event_data_out_channel_halt=0;event_status_channel_halt=0;
  end join
  if(input_halt_count!=1||output_halt_count!=1||status_halt_count!=1||drop_count!=d)$fatal(1,"halt diagnostic behavior");
  // No capacity: no configuration or FFT data may be accepted, deadline drops.
  reserve_ready=0;before_inputs=total_input_handshakes;i=configs;
  send_input(3);reserve_ready=1;
  assert_failure(++d,p,++t);
  if(total_input_handshakes!=before_inputs||configs!=i||last_ingest_cycles!=INGEST)$fatal(1,"full store loaded FFT or missed strict ingest deadline");
  full_job(4);
  // Partial load timeout, drain old remainder locally, then fresh config/frame.
  before_inputs=total_input_handshakes;
  fork send_input(5);begin
   wait(input_count==8);@(negedge clk);fft_s_axis_data_tready=0;
   do @(posedge clk);while(fft_aresetn);
   @(negedge clk);fft_s_axis_data_tready=1;
  end join
  assert_failure(++d,p,++t);
  if(total_input_handshakes-before_inputs!=8)$fatal(1,"failed frame remainder was fed to FFT");
  full_job(6);
  // Source order, TLAST, and metadata are checked before bad transfer.
  offer_word(0,7);offer_word(1,7);
  @(negedge clk);s_valid=1;s_index=3;s_last=0;s_frame_seq=7;
  do @(posedge clk);while(!s_ready);
  @(negedge clk);s_valid=0;
  assert_failure(++d,++p,t);pulse_abort(1);full_job(8);
  offer_word(0,9);offer_word(1,9);
  @(negedge clk);s_valid=1;s_index=2;s_last=1;s_frame_seq=9;
  do @(posedge clk);while(!s_ready);
  @(negedge clk);s_valid=0;
  assert_failure(++d,++p,t);full_job(10);
  offer_word(0,11);offer_word(1,11);
  @(negedge clk);s_valid=1;s_index=2;s_last=0;s_frame_seq=11;s_first_raw=48;
  do @(posedge clk);while(!s_ready);
  @(negedge clk);s_valid=0;s_first_raw=47;
  assert_failure(++d,++p,t);pulse_abort(1);full_job(12);
  // Explicit/source cancellation reset partial load; next identity is not drained.
  offer_word(0,13);offer_word(1,13);pulse_abort(1);
  assert_failure(++d,p,t);full_job(14);
  send_input(15);pulse_abort(0);assert_failure(++d,p,t);full_job(16);
  // A new reader's frame_abort must not cancel the previous completed input.
  send_input(17);pulse_abort(1);send_results(3);send_status(3);repeat(6)@(negedge clk);
  if(drop_count!=d||completed_count!=commits)$fatal(1,"late frame_abort cancelled prior FFT job");
  // Unique status and exponent consistency, including delayed status.
  send_input(18);send_status(3);send_status(3);assert_failure(++d,++p,t);full_job(19);
  send_input(20);send_results(3);send_status(4);assert_failure(++d,++p,t);full_job(21);
  // Padding, early results, and store validation failures reject the private frame.
  send_input(22);@(negedge clk);fft_m_axis_data_tvalid=1;fft_m_axis_data_tuser={8'd3,4'd1,12'd0};
  @(negedge clk);fft_m_axis_data_tvalid=0;assert_failure(++d,++p,t);full_job(23);
  offer_word(0,24);@(negedge clk);fft_m_axis_data_tvalid=1;fft_m_axis_data_tuser=0;
  @(negedge clk);fft_m_axis_data_tvalid=0;assert_failure(++d,++p,t);pulse_abort(1);full_job(25);
  send_input(26);@(negedge clk);fft_m_axis_data_tvalid=1;fft_m_axis_data_tuser={8'd3,4'd0,12'd1};fft_m_axis_data_tlast=0;
  @(negedge clk);fft_m_axis_data_tvalid=0;assert_failure(++d,++p,t);full_job(27);
  // Job/output timeout after complete input resets without draining a new frame.
  send_input(28);do @(posedge clk);while(fft_aresetn);
  assert_failure(++d,p,++t);full_job(29);
  // Strict ingest upper bound: limit-1 accepted, limit rejected before final transfer.
  fork boundary_input(30,INGEST-2);begin send_results(3);send_status(3);end join
  repeat(6)@(negedge clk);
  if(last_ingest_cycles!=INGEST-1)$fatal(1,"strict ingest success boundary wrong");
  before_inputs=total_input_handshakes;boundary_input(31,INGEST-1);assert_failure(++d,p,++t);
  if(total_input_handshakes-before_inputs!=4095)$fatal(1,"deadline raced final transfer");
  full_job(32);
  // Abort and fatal TLAST events win over final input/commit opportunities.
  for(i=0;i<4095;i=i+1)offer_word(i,33);
  @(negedge clk);s_valid=1;s_index=4095;s_last=1;s_frame_seq=33;event_tlast_missing=1;
  @(negedge clk);event_tlast_missing=0;
  do @(posedge clk);while(!s_ready);
  @(negedge clk);s_valid=0;assert_failure(++d,++p,t);full_job(34);
  send_input(35);send_results(3);c=completed_count;
  @(negedge clk);fft_m_axis_status_tdata=3;fft_m_axis_status_tvalid=1;abort=1;
  @(negedge clk);fft_m_axis_status_tvalid=0;abort=0;
  assert_failure(++d,p,t);if(completed_count!=c)$fatal(1,"abort lost final commit race");full_job(36);
  // A status before complete input cannot be borrowed by this later result.
  offer_word(0,37);offer_word(1,37);send_status(3);
  assert_failure(++d,++p,t);pulse_abort(1);full_job(38);
  // A 4097th offered output is illegal even if COMPLETE holds ready low.
  send_input(39);send_results(3);c=completed_count;
  @(negedge clk);fft_m_axis_data_tvalid=1;fft_m_axis_data_tuser={8'd3,4'd0,12'd4095};fft_m_axis_data_tlast=1;
  repeat(2)@(negedge clk);send_status(3);
  @(negedge clk);fft_m_axis_data_tvalid=0;
  assert_failure(++d,++p,t);if(completed_count!=c)$fatal(1,"extra output committed before rejection");full_job(40);
  // Exponent range is checked independently on status and output channels.
  send_input(41);send_status(13);assert_failure(++d,++p,t);full_job(42);
  send_input(43);@(negedge clk);fft_m_axis_data_tvalid=1;fft_m_axis_data_tuser={8'd13,4'd0,12'd0};fft_m_axis_data_tlast=0;
  @(negedge clk);fft_m_axis_data_tvalid=0;assert_failure(++d,++p,t);full_job(44);
  before_inputs=total_input_handshakes;
  for(i=0;i<4095;i=i+1)offer_word(i,45);
  @(negedge clk);s_valid=1;s_index=4095;s_last=1;s_frame_seq=45;frame_abort=1;
  @(negedge clk);s_valid=0;frame_abort=0;
  assert_failure(++d,p,t);if(total_input_handshakes-before_inputs!=4095)$fatal(1,"frame_abort lost final input race");full_job(46);
  $display("PASS fft_scheduler protocol mock commits=%0d drops=%0d protocol=%0d timeouts=%0d resets=%0d",commits,drop_count,protocol_error_count,timeout_count,resets);$finish;
 end
endmodule
