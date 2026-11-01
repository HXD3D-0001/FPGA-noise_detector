// Recoverable source boundary. Requests own capture identity; FAULT must be
// STOPped before a strictly newer START. sample_reset explicitly cancels any
// offered sample on STOP, source fault, new capture, or complete-system reset.
// ADC monitoring uses validated slots, independently of FIFO/output acceptance.
// channel_select and pattern_id are configuration inputs: requester holds them
// stable for the capture; change only while STOPPED before a new capture request.
// origin_raw is a boot seed held constant until the complete-system reset.
module capture_frontend #(
 parameter integer CLOCK_MONITOR_ENABLE=0,FIFO_DEPTH=1024,CLK_HZ=50000000,SAMPLE_HZ=48000,
 RESET_HOLD_CYCLES=8,ADC_WARMUP_CYCLES=100000000,START_TIMEOUT_CYCLES=250000000,DRAIN_TIMEOUT_CYCLES=5000000,
 BCK_TIMEOUT_CYCLES=50000,SLOT_TIMEOUT_CYCLES=500000,TEST_TIMEOUT_CYCLES=500000,
 parameter SINE_FILE="../../generated/pattern_sine.mem"
)(input wire proc_clk,bclk,rst,start_req,stop_req,requested_test_mode,
 input wire mclk_ready_proc,clock_fault_latched,output wire active_test_mode,
 input wire[63:0]next_capture_id,input wire drain_done,channel_select,lrck,sdata,
 input wire[63:0]origin_raw,input wire[1:0]pattern_id,
 output wire running,busy,command_rejected,output wire[63:0]active_capture_id,
 output wire[3:0]state,fault_reason,output reg[63:0]fault_count,output wire sample_reset,source_fault_event,
 output wire m_valid,input wire m_ready,output reg signed[23:0]m_data,
 output reg[63:0]m_capture_id,m_raw_index,output reg[7:0]m_flags,
 output wire[63:0]adc_count,good_slot_count,test_count);
 (* ASYNC_REG="TRUE" *)reg[2:0]proc_reset_pipe,bck_reset_pipe;
 always@(posedge proc_clk or posedge rst)
  if(rst)proc_reset_pipe<=3'b111;else proc_reset_pipe<={proc_reset_pipe[1:0],1'b0};
 always@(posedge bclk or posedge rst)
  if(rst)bck_reset_pipe<=3'b111;else bck_reset_pipe<={bck_reset_pipe[1:0],1'b0};
 wire pr=rst|proc_reset_pipe[2],br=rst|bck_reset_pipe[2];
 wire test_enable,adc_enable,source_gate,fifo_reset,request_toggle,pattern_start;
 wire clock_unavailable=CLOCK_MONITOR_ENABLE ? (!mclk_ready_proc||clock_fault_latched) : 1'b0;
 wire qualified_start=start_req&&(requested_test_mode||!clock_unavailable);
 wire accepted_start=qualified_start&&!stop_req&&state==0&&next_capture_id!=0&&next_capture_id>active_capture_id;
 wire controller_rejected;
 assign command_rejected=controller_rejected||(start_req&&!requested_test_mode&&clock_unavailable);
 wire adc_enable_source;
 (* ASYNC_REG="TRUE" *)reg[1:0]channel_sync;
 wire adc_reset_done,ack_valid,ack_toggle;wire[63:0]ack_capture,source_capture;
 wire source_reset,aligned,rx_valid,rx_ready,adc_valid,adc_ready,pattern_valid,pattern_ready;
 wire signed[23:0]rx_data,adc_data,pattern_data;
 wire[63:0]rx_capture,rx_index,adc_capture,adc_index,pattern_capture,pattern_index;
 wire[7:0]rx_flags,adc_flags,pattern_flags;
 wire[63:0]physical_count,valid_slots,format_count,adc_drops,clips,test_drops;
 reg[63:0]bck_count,bck_gray,slot_gray,physical_gray,format_gray;
 (* ASYNC_REG="TRUE" *)reg[63:0]bck_meta,bck_sync,physical_meta,physical_sync,slot_meta,slot_sync,format_meta,format_sync;
 reg[63:0]last_format;
 reg[63:0]warmup_raw_floor;
 reg discard_warmup_tail;wire discard_adc_tail;
 function[63:0]ungray;input[63:0]g;integer k;begin ungray[63]=g[63];for(k=62;k>=0;k=k-1)ungray[k]=ungray[k+1]^g[k];end endfunction
 always@(posedge bclk or posedge br)begin
  if(br)begin channel_sync<=0;bck_count<=0;bck_gray<=0;slot_gray<=0;physical_gray<=0;format_gray<=0;end
  else begin
   channel_sync<={channel_sync[0],channel_select};
   bck_count<=bck_count+1'b1;bck_gray<=((bck_count+1'b1)>>1)^(bck_count+1'b1);
   slot_gray<=(valid_slots>>1)^valid_slots;physical_gray<=(physical_count>>1)^physical_count;format_gray<=(format_count>>1)^format_count;
  end
 end
 always@(posedge proc_clk or posedge pr)begin
  if(pr)begin bck_meta<=0;bck_sync<=0;physical_meta<=0;physical_sync<=0;slot_meta<=0;slot_sync<=0;format_meta<=0;format_sync<=0;last_format<=0;warmup_raw_floor<=0;discard_warmup_tail<=0;end
  else begin
   bck_meta<=bck_gray;bck_sync<=bck_meta;
   physical_meta<=physical_gray;physical_sync<=physical_meta;slot_meta<=slot_gray;slot_sync<=slot_meta;
   format_meta<=format_gray;format_sync<=format_meta;last_format<=format_sync;
   // The lifetime progress bus trails the source by a registered Gray stage
   // and two proc stages. At 48 kHz/50 MHz this is far below one slot period;
   // one extra slot excludes the last settling slot, including CDC tail data.
   if(adc_enable&&!running)begin warmup_raw_floor<=origin_raw+ungray(physical_sync)+1'b1;discard_warmup_tail<=1;end
   else if(running&&adc_valid&&adc_ready&&!discard_adc_tail)discard_warmup_tail<=0;
  end
 end
 assign adc_count=ungray(physical_sync);assign good_slot_count=ungray(slot_sync);
 i2s_rx rx(.bclk(bclk),.rst(source_reset),.counter_rst(br),.enable(adc_enable_source),.capture_id(source_capture),.origin_raw(origin_raw),
  .channel_select(channel_sync[1]),.lrck(lrck),.sdata(sdata),.m_valid(rx_valid),.m_ready(rx_ready),.m_data(rx_data),
  .m_capture_id(rx_capture),.m_raw_index(rx_index),.m_flags(rx_flags),.adc_count(physical_count),.drop_count(adc_drops),
  .format_error_count(format_count),.clip_count(clips),.slot_aligned(aligned),.good_slot_count(valid_slots));
 audio_cdc #(.FIFO_DEPTH(FIFO_DEPTH)) cdc(.proc_clk(proc_clk),.source_clk(bclk),.rst(pr),.fifo_reset(fifo_reset),
  .source_req_toggle(request_toggle),.requested_capture_id(active_capture_id),.source_enable(adc_enable),.source_aligned(aligned),
  .source_capture_id(source_capture),.source_ack_valid(ack_valid),.source_ack_toggle(ack_toggle),.source_ack_capture_id(ack_capture),
  .reset_done(adc_reset_done),.source_reset(source_reset),.source_enable_synced(adc_enable_source),.s_valid(rx_valid),.s_ready(rx_ready),.s_data(rx_data),
  .s_capture_id(rx_capture),.s_raw_index(rx_index),.s_flags(rx_flags),.m_valid(adc_valid),.m_ready(adc_ready),
  .m_data(adc_data),.m_capture_id(adc_capture),.m_raw_index(adc_index),.m_flags(adc_flags));
 pattern_source #(.CLK_HZ(CLK_HZ),.SAMPLE_HZ(SAMPLE_HZ),.SINE_FILE(SINE_FILE)) pattern(.clk(proc_clk),.rst(pr),.enable(test_enable),.start(pattern_start),
  .capture_id(active_capture_id),.origin_raw(origin_raw),.pattern_id(pattern_id),.m_valid(pattern_valid),.m_ready(pattern_ready),
  .m_data(pattern_data),.m_capture_id(pattern_capture),.m_raw_index(pattern_index),.m_flags(pattern_flags),.raw_count(test_count),.drop_count(test_drops));
 wire watchdog_fault;wire[3:0]watchdog_reason;wire[63:0]watchdog_count;
 input_watchdog #(.PROGRESS_SYNCHRONIZED(1),.BCK_TIMEOUT_CYCLES(BCK_TIMEOUT_CYCLES),.SLOT_TIMEOUT_CYCLES(SLOT_TIMEOUT_CYCLES),.TEST_TIMEOUT_CYCLES(TEST_TIMEOUT_CYCLES)) watchdog(
  .proc_clk(proc_clk),.rst(pr),.rearm(accepted_start),.enable(running),.test_mode(test_enable),.bck_progress_gray(bck_sync),
  .slot_progress_gray(slot_sync),.test_progress(test_count),.format_fault(format_sync!=last_format),
  .fault(watchdog_fault),.fault_reason(watchdog_reason),.fault_count(watchdog_count));
 assign discard_adc_tail=discard_warmup_tail&&!test_enable&&adc_valid&&adc_index<warmup_raw_floor;
 wire selected_valid=test_enable?pattern_valid:(adc_valid&&!discard_adc_tail);
 wire signed[23:0]selected_data=test_enable?pattern_data:adc_data;
 wire[63:0]selected_capture=test_enable?pattern_capture:adc_capture,selected_index=test_enable?pattern_index:adc_index;
 wire[7:0]selected_flags=test_enable?pattern_flags:adc_flags;
 wire selected_ready,guard_valid,guard_ready,history_reset;
 wire signed[23:0]guard_data;wire[63:0]guard_capture,guard_index,gap_count,stale_count;wire[7:0]guard_flags;
 // WAIT_ACK/WARMUP must consume independently of downstream backpressure:
 // two seconds of ADC settling cannot accumulate in a 21 ms source FIFO.
 assign adc_ready=adc_enable&&(!running||discard_adc_tail||selected_ready);
 assign pattern_ready=test_enable&&selected_ready;
 stream_guard guard(.clk(proc_clk),.rst(pr),.enable(running),.expected_capture_id(active_capture_id),
  .s_valid(selected_valid),.s_ready(selected_ready),.s_data(selected_data),.s_capture_id(selected_capture),.s_raw_index(selected_index),.s_flags(selected_flags),
  .m_valid(guard_valid),.m_ready(guard_ready),.m_data(guard_data),.m_capture_id(guard_capture),.m_raw_index(guard_index),.m_flags(guard_flags),
  .history_reset(history_reset),.gap_count(gap_count),.stale_count(stale_count));
 wire stream_fault=running&&selected_valid&&((selected_capture!=active_capture_id)||(guard_flags[2]||guard_flags[4]));
 wire clock_fault=!active_test_mode&&clock_unavailable;
 wire source_fault=clock_fault||watchdog_fault||stream_fault;
 wire[3:0]source_reason=clock_fault?4'd9:(watchdog_fault?watchdog_reason:4'd8);
 capture_controller #(.RESET_HOLD_CYCLES(RESET_HOLD_CYCLES),.ADC_WARMUP_CYCLES(ADC_WARMUP_CYCLES),.TIMEOUT_CYCLES(START_TIMEOUT_CYCLES),.DRAIN_TIMEOUT_CYCLES(DRAIN_TIMEOUT_CYCLES)) controller(
  .clk(proc_clk),.rst(pr),.start_req(qualified_start),.stop_req(stop_req),.requested_test_mode(requested_test_mode),.next_capture_id(next_capture_id),
  .drain_done(drain_done),.adc_reset_done(adc_reset_done),.adc_ack_valid(ack_valid),.adc_ack_toggle(ack_toggle),.adc_ack_capture_id(ack_capture),
  .active_test_mode(active_test_mode),.running(running),.test_enable(test_enable),.adc_enable(adc_enable),.source_gate(source_gate),.fifo_reset(fifo_reset),
  .source_req_toggle(request_toggle),.pattern_start(pattern_start),.active_capture_id(active_capture_id),.state(state),.fault_reason(fault_reason),
  .busy(busy),.command_rejected(controller_rejected),.source_fault(source_fault),.source_fault_reason(source_reason));
 reg held_valid,previous_fault;
 // START invalidates state at the accepting edge; it must not feed the
 // combinational sample/ready path used to decide that START is permitted.
 assign source_fault_event=(running&&source_fault)||(state==7&&!previous_fault);
 wire live_cancel=pr||(stop_req&&state!=0)||source_fault_event;
 assign sample_reset=live_cancel||accepted_start;
 assign m_valid=held_valid&&running&&!live_cancel;
 assign guard_ready=running&&(!held_valid||m_ready)&&!source_fault&&!live_cancel;
 always@(posedge proc_clk or posedge pr)begin
  if(pr)begin held_valid<=0;m_data<=0;m_capture_id<=0;m_raw_index<=0;m_flags<=0;previous_fault<=0;fault_count<=0;end
  else begin
   previous_fault<=state==7;
   if(state==7&&!previous_fault)fault_count<=fault_count+1'b1;
   if(sample_reset)held_valid<=0;
   else begin
    if(m_valid&&m_ready)held_valid<=0;
    if(guard_valid&&guard_ready)begin held_valid<=1;m_data<=guard_data;m_capture_id<=guard_capture;m_raw_index<=guard_index;m_flags<=guard_flags;end
   end
  end
 end
endmodule
