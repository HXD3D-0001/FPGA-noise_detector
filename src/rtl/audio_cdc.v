// XPM async FIFO carries {signed24,capture64,raw64,flags8}=160 bits.
// requested_capture_id is a bundled-data bus: hold it stable from toggling the
// request until ACK, and while enabled. source_aligned must represent a validated
// complete I2S slot for the new capture. Physical sample loss is counted upstream.
module audio_cdc #(parameter integer FIFO_DEPTH=1024)(
 input wire proc_clk,source_clk,rst,fifo_reset,source_req_toggle,
 input wire[63:0]requested_capture_id,input wire source_enable,source_aligned,
 output reg[63:0]source_capture_id,output wire source_ack_valid,source_ack_toggle,
 output wire[63:0]source_ack_capture_id,output wire reset_done,source_reset,source_enable_synced,
 input wire s_valid,output wire s_ready,input wire signed[23:0]s_data,
 input wire[63:0]s_capture_id,s_raw_index,input wire[7:0]s_flags,
 output wire m_valid,input wire m_ready,output wire signed[23:0]m_data,
 output wire[63:0]m_capture_id,m_raw_index,output wire[7:0]m_flags);
 // rst may assert asynchronously; capture control is synchronous to proc_clk.
 // Register its decode before crossing to the source reset release chain.
 reg reset_request,enable_proc;
 always@(posedge proc_clk or posedge rst)
  if(rst)reset_request<=1'b1;else reset_request<=fifo_reset;
 always@(posedge proc_clk or posedge reset_request)
  if(reset_request)enable_proc<=0;else enable_proc<=source_enable;
 // Async assert, source-clock release: stopped BCK cannot consume/reset ACK.
 (* ASYNC_REG="TRUE" *)reg[2:0]source_reset_pipe;
 always@(posedge source_clk or posedge reset_request)
  if(reset_request)source_reset_pipe<=3'b111;else source_reset_pipe<={source_reset_pipe[1:0],1'b0};
 wire xpm_reset=source_reset_pipe[2];
 assign source_reset=xpm_reset;
 wire full,empty,wr_busy,rd_busy;wire[159:0]dout;
 // A fault may assert logical reset again while XPM is still completing the
 // preceding reset. No writes/ACKs are allowed in that interval, so that
 // request is redundant until busy drops. Coalesce the physical reset edge;
 // the logical async-assert/source-release pipe above remains authoritative.
 // Boot reset also releases through an explicit source-domain synchronizer;
 // a raw proc-domain reset must not drive the coalescer's async release.
 reg boot_reset_request;
 always@(posedge proc_clk or posedge rst)
  if(rst)boot_reset_request<=1;else boot_reset_request<=0;
 (* ASYNC_REG="TRUE" *)reg[2:0]boot_reset_pipe;
 always@(posedge source_clk or posedge boot_reset_request)
  if(boot_reset_request)boot_reset_pipe<=3'b111;else boot_reset_pipe<={boot_reset_pipe[1:0],1'b0};
 wire fifo_boot_reset=boot_reset_pipe[2];
 reg fifo_reset_active;
 always@(posedge source_clk or posedge fifo_boot_reset)begin
  if(fifo_boot_reset)fifo_reset_active<=1;
  else if(fifo_reset_active)begin
   if(!xpm_reset)fifo_reset_active<=0;
  end else if(xpm_reset&&!wr_busy)fifo_reset_active<=1;
 end
 (* ASYNC_REG="TRUE" *)reg[1:0]req_sync,enable_sync;
 reg loaded,ack_source_valid,ack_source_toggle,source_clear;
 always@(posedge source_clk or posedge xpm_reset)begin
  if(xpm_reset)begin req_sync<=0;enable_sync<=0;loaded<=0;ack_source_valid<=0;ack_source_toggle<=0;source_capture_id<=0;source_clear<=0;end
  else begin
   req_sync<={req_sync[0],source_req_toggle};enable_sync<={enable_sync[0],enable_proc};
   if(xpm_reset||wr_busy)begin loaded<=0;ack_source_valid<=0;source_clear<=0;end
   else begin
    source_clear<=1;
    if(!loaded||req_sync[1]!=ack_source_toggle)begin
     source_capture_id<=requested_capture_id;ack_source_toggle<=req_sync[1];loaded<=1;ack_source_valid<=0;
    end else if(source_aligned)ack_source_valid<=1;
   end
  end
 end
 (* ASYNC_REG="TRUE" *)reg[1:0]clear_sync,ack_valid_sync,ack_toggle_sync;
 reg[63:0]ack_capture_proc;
 always@(posedge proc_clk or posedge reset_request)begin
  if(reset_request)begin clear_sync<=0;ack_valid_sync<=0;ack_toggle_sync<=0;ack_capture_proc<=0;end
  else begin
   clear_sync<={clear_sync[0],source_clear};ack_valid_sync<={ack_valid_sync[0],ack_source_valid};ack_toggle_sync<={ack_toggle_sync[0],ack_source_toggle};
   // The identity was latched before the source raised ACK and has had two
   // proc synchronizer cycles to settle. It is a handshake bus, not a Gray bus.
   if(ack_valid_sync[1])ack_capture_proc<=source_capture_id;
  end
 end
 assign reset_done=!rst&&!fifo_reset&&!reset_request&&clear_sync[1]&&!rd_busy;
 assign source_enable_synced=enable_sync[1];
 assign source_ack_valid=reset_done&&ack_valid_sync[1]&&ack_capture_proc==requested_capture_id&&ack_toggle_sync[1]==source_req_toggle;
 assign source_ack_toggle=ack_toggle_sync[1];assign source_ack_capture_id=ack_capture_proc;
 assign s_ready=!xpm_reset&&!wr_busy&&!full&&enable_sync[1]&&ack_source_valid&&s_capture_id==source_capture_id;
 assign m_valid=source_ack_valid&&!empty;
 assign {m_data,m_capture_id,m_raw_index,m_flags}=dout;
 xpm_fifo_async #(.FIFO_MEMORY_TYPE("auto"),.FIFO_WRITE_DEPTH(FIFO_DEPTH),
  .WRITE_DATA_WIDTH(160),.READ_DATA_WIDTH(160),.READ_MODE("fwft"),.FIFO_READ_LATENCY(0),
  .CDC_SYNC_STAGES(2),.USE_ADV_FEATURES("0000"),.SIM_ASSERT_CHK(1)) fifo(
  .rst(fifo_reset_active),.wr_clk(source_clk),.rd_clk(proc_clk),.din({s_data,s_capture_id,s_raw_index,s_flags}),
  .wr_en(s_valid&&s_ready),.rd_en(m_valid&&m_ready),.dout(dout),.full(full),.empty(empty),
  .wr_rst_busy(wr_busy),.rd_rst_busy(rd_busy),.sleep(1'b0),.injectsbiterr(1'b0),.injectdbiterr(1'b0),
  .prog_full(),.wr_data_count(),.overflow(),.almost_full(),.wr_ack(),.prog_empty(),.rd_data_count(),
  .underflow(),.almost_empty(),.data_valid(),.sbiterr(),.dbiterr());
endmodule
