`timescale 1ns/1ps
module tb_spectrum_shared_store;
 reg clk=0;always #5 clk=~clk;
 reg rst=1,reserve_valid=0,abort=0,commit=0,result_valid=0;
 wire reserve_ready,result_ready,collect_done,collect_error;
 reg [63:0] reserve_capture_id=64'h200000003,reserve_first_raw=64'h100000000;
 reg [63:0] reserve_frame_seq=64'h100000001;
 reg [31:0] reserve_config_id=32'h87654321;reg [7:0] reserve_flags=1;
 reg signed [23:0] result_real=0,result_imag=0;
 reg [11:0] result_index=0;reg [7:0] result_blk_exp=5;reg result_last=0;
 wire [7:0] collect_blk_exp;
 wire packet_valid;reg packet_ready=0,packet_done=0;
 wire [15:0] packet_type,packet_flags;wire [63:0] packet_capture_id,packet_seq,packet_first_raw;
 wire [31:0] packet_config_id;wire [7:0] packet_blk_exp;
 reg read_en=0;reg [12:0] read_addr=0;wire read_valid;wire [31:0] read_data;
 wire [63:0] completed_count,drop_count,invalid_count;
 reg mode_valid=0,stopped=1,flush_req=0,dma_reset_done=0,raw_valid=0,raw_analysis_reset=0;
 reg [1:0] mode_select=0; wire[1:0]mode_active;wire mode_ready,banks_idle,mode_rejected,flush_pending,flush_done,flush_timeout,raw_ready;
 wire[31:0]flush_request_seq;reg[31:0]dma_reset_seq=0;
 reg signed[23:0]raw_data=0;reg[63:0]raw_capture_id=64'h8000000200000006,raw_index=0;
 reg[31:0]raw_config_id=2;reg[7:0]raw_flags=1;
 wire[63:0]raw_candidate_count,raw_completed_count,raw_drop_count,raw_invalid_count;
 spectrum_shared_store #(.FLUSH_TIMEOUT_TICKS(20)) dut(.grid_meta_valid(),.grid_meta_capture(),.grid_meta_config(),.grid_meta_seq(),.grid_meta_raw(),.*);
 reg held=0;reg [271:0] held_descriptor;
 wire [271:0] descriptor_bits={packet_type,packet_flags,packet_capture_id,packet_seq,
                             packet_first_raw,packet_config_id,packet_blk_exp,8'd0};
 always @(posedge clk)begin
  if(rst)held<=0;
  else begin
   if(held&&!flush_pending&&!flush_req&&(!packet_valid||descriptor_bits!==held_descriptor))$fatal(1,"stalled descriptor changed/withdrawn");
   held<=packet_valid&&!packet_ready;
   held_descriptor<=descriptor_bits;
  end
 end
 function automatic signed [23:0] value(input integer i,input integer token,input bit imag);
  if(i%8==0)value=imag?24'sh7fffff:24'sh800000;
  else if(i%8==1)value=imag?-24'sd1:24'sd0;
  else value=imag?-(i*37+token*101):(i*655+token*97);
 endfunction
 task automatic tick;@(posedge clk);#1;endtask
 task automatic reserve(input logic [63:0] c,s,r,input logic [31:0] cfg,input logic [7:0] f,input bit good);
  @(negedge clk);reserve_capture_id=c;reserve_frame_seq=s;reserve_first_raw=r;reserve_config_id=cfg;reserve_flags=f;reserve_valid=1;
  #1;if(!reserve_ready)$fatal(1,"reserve unavailable");tick;
  if(collect_error!==!good)$fatal(1,"reserve rejection mismatch");
  @(negedge clk);reserve_valid=0;
 endtask
 task automatic point(input integer i,t,input integer exp,input bit last,input bit bad);
  @(negedge clk);result_index=i;result_real=value(i,t,0);result_imag=value(i,t,1);result_blk_exp=exp;result_last=last;result_valid=1;
  #1;if(!result_ready)$fatal(1,"result unavailable");tick;
  if(collect_error!==bad)$fatal(1,"point rejection mismatch at %d",i);
  @(negedge clk);result_valid=0;
 endtask
 task automatic fill(input integer token,exp);
  for(integer i=0;i<4096;i++)point(i,token,exp,i==4095,0);
  if(!collect_done||collect_blk_exp!=exp)$fatal(1,"complete/exponent missing");
 endtask
 task automatic publish;
  @(negedge clk);commit=1;tick;@(negedge clk);commit=0;
  if(collect_done)$fatal(1,"private complete retained aftercommit");
 endtask
 task automatic counts(input logic [63:0] c,d,n);
  if(completed_count!=c||drop_count!=d||invalid_count!=n)
   $fatal(1,"counts got %d %d %d expected %d %d %d",completed_count,drop_count,invalid_count,c,d,n);
 endtask
 task automatic descriptor(input logic [63:0] c,s,r,input logic [31:0] cfg,input logic [7:0] f,e);
  if(!packet_valid||packet_type!=1||packet_capture_id!=c||packet_seq!=s||packet_first_raw!=r||packet_config_id!=cfg||packet_flags!={8'd0,f}||packet_blk_exp!=e)
   $fatal(1,"packet descriptor mismatch seq=%h flags=%h",packet_seq,packet_flags);
 endtask
 task automatic take;
  @(negedge clk);packet_ready=1;tick;@(negedge clk);packet_ready=0;
  if(packet_valid)$fatal(1,"reader owner published twice");
 endtask
 task automatic read_all(input integer token);
  reg [31:0] previous_data;reg previous_valid;
  for(integer j=0;j<8192;j++)begin
   @(negedge clk);previous_data=read_data;previous_valid=read_valid;read_en=1;read_addr=j;
   // The previous registered response must remain stable before the next clock.
   #1;if(read_data!==previous_data||read_valid!==previous_valid)$fatal(1,"read port is asynchronous");
   tick;
   if(!read_valid||$signed(read_data)!=$signed(value(j/2,token,j%2)))$fatal(1,"read value mismatch %d got%h",j,read_data);
  end
  @(negedge clk);read_en=0;tick;if(read_valid)$fatal(1,"read_valid not registered request");
 endtask
 task automatic release_packet;
  @(negedge clk);packet_done=1;tick;@(negedge clk);packet_done=0;
 endtask
 task automatic flush_banks;
  @(negedge clk);flush_req=1;tick;@(negedge clk);flush_req=0;
  if(!flush_pending)$fatal(1,"flush request missing");
  dma_reset_seq=flush_request_seq;dma_reset_done=1;tick;
  if(!flush_done||flush_pending||!banks_idle)$fatal(1,"matching flush did not complete");
  @(negedge clk);dma_reset_done=0;tick;
 endtask
 task automatic send_raw(input integer start,count,token);
  for(integer i=0;i<count;i++)begin
   @(negedge clk);raw_valid=1;raw_index=start+i;raw_data=value(i,token,0);
   #1;if(!raw_ready)$fatal(1,"RAW input backpressured");tick;
  end
  @(negedge clk);raw_valid=0;
 endtask
 task automatic read_raw(input integer token);
  for(integer j=0;j<8192;j++)begin
   @(negedge clk);read_en=1;read_addr=j;tick;
   if(!read_valid||$signed(read_data)!=$signed(value(j,token,0)))$fatal(1,"RAW value mismatch %d %h",j,read_data);
  end
  @(negedge clk);read_en=0;tick;
 endtask
 initial begin
  repeat(3)tick;@(negedge clk);rst=0;tick;
  // Boot ownership RESET can repeat before any FFT has existed.
  flush_banks;flush_banks;counts(0,0,0);
  if(!reserve_ready)$fatal(1,"empty bank must accept reservation");
  read_en=1;tick;if(read_valid)$fatal(1,"read without owner");read_en=0;
  reserve(64'h200000003,64'h100000001,64'h100000000,32'h87654321,8'h09,1);
  fill(1,5);if(packet_valid)$fatal(1,"private COMPLETE prematurelypublished");repeat(4)tick;
  if(!collect_done)$fatal(1,"collect_done not held");publish;
  descriptor(64'h200000003,64'h100000001,64'h100000000,32'h87654321,8'h09,5);
  repeat(5)begin tick;descriptor(64'h200000003,64'h100000001,64'h100000000,32'h87654321,8'h09,5);end
  take;
  reserve(64'h200000003,64'h100000002,64'h100030000,32'h87654321,1,1);fill(2,12);publish;counts(2,0,0);
  @(negedge clk);reserve_valid=1;reserve_frame_seq=64'h100000003;tick;if(reserve_ready)$fatal(1,"full banks accepted");
  @(negedge clk);reserve_valid=0;abort=1;tick;@(negedge clk);abort=0;counts(2,0,0);
  read_all(1); // New READY bank must not replace the reader's data.
  @(negedge clk);packet_done=1;reserve_valid=1;reserve_frame_seq=64'h100000003;reserve_first_raw=64'h100060000;
  #1;if(!reserve_ready)$fatal(1,"simultaneousrelease/reserve unavailable");tick;
  @(negedge clk);packet_done=0;reserve_valid=0;abort=1;tick;@(negedge clk);abort=0;counts(2,1,0);
  descriptor(64'h200000003,64'h100000002,64'h100030000,32'h87654321,1,12);take;read_all(2);release_packet;
  reserve(64'h200000002,64'h100000004,0,32'h87654321,1,0);counts(2,1,1);
  reserve(64'h200000003,64'h100000004,0,32'h87654322,1,0);counts(2,1,2);
  @(negedge clk);abort=1;tick;@(negedge clk);abort=0;
  reserve(64'h200000003,64'h100000004,0,32'h87654321,1,0);
  reserve(64'h200000002,64'h100000004,0,32'h87654323,1,0);
  reserve(64'h200000003,64'h100000004,0,32'h87654321,1,0);counts(2,1,5);
  reserve(64'h200000004,64'h100000004,64'h100090000,32'h87654323,1,1);fill(3,0);publish;
  descriptor(64'h200000004,64'h100000004,64'h100090000,32'h87654323,5,0);
  reserve(64'h200000004,64'h100000004,0,32'h87654323,1,0);counts(3,1,6);
  reserve(64'h200000004,64'h100000005,0,32'h87654323,1,1);point(1,4,1,0,1);counts(3,2,7);
  reserve(64'h200000004,64'h100000006,0,32'h87654323,1,1);point(0,4,1,1,1);counts(3,3,8);
  reserve(64'h200000004,64'h100000007,0,32'h87654323,1,1);point(0,4,13,0,1);counts(3,4,9);
  reserve(64'h200000004,64'h100000008,0,32'h87654323,1,1);point(0,4,1,0,0);point(1,4,2,0,1);counts(3,5,10);
  reserve(64'h200000004,64'h100000009,0,32'h87654323,1,1);
  for(integer i=0;i<4096;i++)point(i,4,1,0,i==4095);counts(3,6,11);
  reserve(64'h200000004,64'h10000000a,0,32'h87654323,1,1);
  @(negedge clk);commit=1;tick;if(!collect_error)$fatal(1,"earlycommit accepted");@(negedge clk);commit=0;counts(3,7,12);
  reserve(64'h200000004,64'h10000000b,0,32'h87654323,1,1);fill(4,3);
  @(negedge clk);abort=1;commit=1;tick;@(negedge clk);abort=0;commit=0;counts(3,8,12);
  reserve(0,64'h10000000c,0,1,1,0);
  reserve(64'h200000005,64'h10000000c,0,0,1,0);
  reserve(64'h200000005,64'h10000000c,0,1,8'h81,0);
  reserve(64'h200000005,64'h10000000c,0,1,3,0);
  reserve(64'h200000005,64'h10000000c,0,1,0,0);
  reserve(64'h200000005,64'h10000000c,64'hffffffffffff0000,1,1,0);counts(3,8,18);
  reserve(64'h200000004,64'h10000000b,0,32'h87654323,1,0); // Aborted sequence cannot replay.
  reserve(64'h200000002,64'h10000000c,0,32'h87654324,1,0);
  reserve(64'h200000002,64'h10000000c,0,32'h87654325,1,0);counts(3,8,21);
  descriptor(64'h200000004,64'h100000004,64'h100090000,32'h87654323,5,0);take;read_all(3);release_packet;
  reserve(64'h8000000200000005,64'h800000010000000c,64'hfffffffffff00000,1,1,1);fill(5,11);publish;
  descriptor(64'h8000000200000005,64'h800000010000000c,64'hfffffffffff00000,1,5,11);take;
  reserve(64'h8000000200000005,64'h800000010000000d,0,1,8'h31,1);fill(6,7);publish;
  read_all(5);release_packet;
  // Sticky GAP clears only on successful commit; all nonreserved flags survive.
  descriptor(64'h8000000200000005,64'h800000010000000d,0,1,8'h31,7);take;read_all(6);release_packet;counts(5,8,21);
  reserve(64'h8000000200000005,64'h800000010000000e,0,1,1,1);
  for(integer i=0;i<4095;i++)point(i,7,2,0,0);
  @(negedge clk);result_valid=1;result_index=4095;result_last=1;abort=1;
  #1;if(result_ready)$fatal(1,"abort did not revoke private result handshake");tick;
  @(negedge clk);abort=0;result_valid=0;tick;
  if(collect_done||packet_valid||collect_error)$fatal(1,"abort on finalpoint published/rejected twice");counts(5,9,21);
  // The exact FFT contract above must coexist with RAW on the same two banks.
  @(negedge clk);mode_select=1;mode_valid=1;stopped=0;tick;
  if(!mode_rejected||mode_active!=0)$fatal(1,"running mode change accepted");
  @(negedge clk);stopped=1;reserve_valid=1;#1;
  if(reserve_ready)$fatal(1,"mode change and FFT reserve accepted on same edge");
  tick;@(negedge clk);mode_valid=0;reserve_valid=0;
  if(mode_active!=1||reserve_ready||result_ready)$fatal(1,"RAW mode not exclusive");
  send_raw(0,8192,1);if(!packet_valid||packet_type!=3||packet_blk_exp!=0||packet_first_raw!=0||packet_seq!=0)$fatal(1,"RAW metadata");
  if(packet_flags!=1)$fatal(1,"normal first new RAW identity falsely marked GAP");
  repeat(4)tick;take;send_raw(8192,8192,2);send_raw(16384,8192,3);
  if(raw_candidate_count!=3||raw_completed_count!=2||raw_drop_count!=1)$fatal(1,"RAW fullpool did not drop exact candidate");
  read_raw(1);release_packet;take;read_raw(2);release_packet;
  send_raw(24576,8192,4);if(packet_first_raw!=24576||packet_seq!=3||!packet_flags[2])$fatal(1,"RAW failed candidate sequence/gap");
  @(negedge clk);mode_select=0;mode_valid=1;tick;if(!mode_rejected||mode_active!=1)$fatal(1,"owned modechange accepted");@(negedge clk);mode_valid=0;
  take;read_en=1;read_addr=1;tick;read_en=0;
  @(negedge clk);flush_req=1;tick;@(negedge clk);flush_req=0;
  if(!flush_pending||mode_ready||raw_ready||packet_valid)$fatal(1,"flush did not quarantine");
  dma_reset_seq=flush_request_seq-1;dma_reset_done=1;tick;dma_reset_done=0;tick;if(!flush_pending)$fatal(1,"stale DMA reset freed banks");
  repeat(25)tick;if(!flush_timeout||!flush_pending)$fatal(1,"timeout released DMA owned bank");
  dma_reset_seq=flush_request_seq;dma_reset_done=1;tick;dma_reset_done=0;tick;
  if(flush_pending||!mode_ready||packet_valid)$fatal(1,"matching reset failed flush");
  @(negedge clk);mode_valid=1;mode_select=0;tick;@(negedge clk);mode_valid=0;
  if(mode_active!=0||raw_ready)$fatal(1,"FFT mode switch failed");
  reserve(64'h8000000200000007,64'h800000010000000f,0,3,1,1);fill(8,2);publish;take;
  fork
   read_all(8);
   begin reserve(64'h8000000200000007,64'h8000000100000010,196608,3,1,1);fill(9,2);publish;end
  join
  release_packet;take;read_all(9);release_packet;
  @(negedge clk);mode_valid=1;mode_select=2;tick;@(negedge clk);mode_valid=0;raw_capture_id=64'h8000000200000008;raw_config_id=4;
  send_raw(0,400,1);raw_analysis_reset=1;tick;raw_analysis_reset=0;tick;
  if(packet_valid)$fatal(1,"partial RAW published on reset");send_raw(8192,8192,5);take;read_raw(5);release_packet;
  if(raw_candidate_count!=6||raw_completed_count!=4||raw_drop_count!=3)$fatal(1,"RAW recovery counts %d %d %d",raw_candidate_count,raw_completed_count,raw_drop_count);
  // A real producer GAP survives even when the descriptor changes identity.
  raw_capture_id=64'h8000000200000009;
  for(integer gap_i=0;gap_i<8192;gap_i=gap_i+1)begin
   @(negedge clk);raw_valid=1;raw_index=32768+gap_i;raw_data=value(gap_i,6,0);raw_flags=gap_i==0?5:1;
   #1;if(!raw_ready)$fatal(1,"RAW producer GAP input backpressured");tick;
  end
  @(negedge clk);raw_valid=0;
  if(!packet_valid||packet_flags!=5)$fatal(1,"real RAW producer GAP was suppressed");
  take;read_raw(6);release_packet;
  if(raw_candidate_count!=7||raw_completed_count!=5||raw_drop_count!=3)$fatal(1,"RAW real GAP accounting");
  // Empty RESET after a clean drain must not mark the next capture as lost.
  @(negedge clk);mode_valid=1;mode_select=0;tick;@(negedge clk);mode_valid=0;
  flush_banks;
  reserve(64'h800000020000000a,64'h8000000100000011,0,5,1,1);fill(10,2);publish;
  descriptor(64'h800000020000000a,64'h8000000100000011,0,5,1,2);take;release_packet;
  flush_banks;
  reserve(64'h800000020000000b,64'h8000000100000012,0,6,1,1);fill(11,2);publish;
  descriptor(64'h800000020000000b,64'h8000000100000012,0,6,1,2);take;release_packet;
  // A discarded private frame leaves GAP sticky across another empty RESET.
  reserve(64'h800000020000000b,64'h8000000100000013,196608,6,1,1);point(0,12,2,0,0);
  flush_banks;flush_banks;
  reserve(64'h800000020000000c,64'h8000000100000014,0,7,1,1);fill(12,2);publish;
  descriptor(64'h800000020000000c,64'h8000000100000014,0,7,5,2);
  // Published and DMA-owned frames also count as real loss on RESET.
  flush_banks;
  reserve(64'h800000020000000c,64'h8000000100000015,196608,7,1,1);fill(13,2);publish;
  descriptor(64'h800000020000000c,64'h8000000100000015,196608,7,5,2);take;
  flush_banks;flush_banks;
  reserve(64'h800000020000000c,64'h8000000100000016,393216,7,1,1);fill(14,2);publish;
  descriptor(64'h800000020000000c,64'h8000000100000016,393216,7,5,2);take;release_packet;
  // COMPLETE is private but still represents a real frame discarded by RESET.
  reserve(64'h800000020000000c,64'h8000000100000017,589824,7,1,1);fill(15,2);
  flush_banks;flush_banks;
  reserve(64'h800000020000000c,64'h8000000100000018,786432,7,1,1);fill(16,2);publish;
  descriptor(64'h800000020000000c,64'h8000000100000018,786432,7,5,2);take;release_packet;
  counts(13,13,21);
  flush_banks;
  reserve(64'h800000020000000c,64'h8000000100000019,983040,7,1,1);fill(17,2);publish;
  descriptor(64'h800000020000000c,64'h8000000100000019,983040,7,1,2);take;release_packet;
  // An upstream loss can leave GAP with no bank owned. Empty flush preserves it.
  @(negedge clk);abort=1;tick;@(negedge clk);abort=0;
  flush_banks;
  reserve(64'h800000020000000c,64'h800000010000001a,1179648,7,1,1);fill(18,2);publish;
  descriptor(64'h800000020000000c,64'h800000010000001a,1179648,7,5,2);counts(15,13,21);
  $display("PASS spectrum_shared_store");$finish;
 end
 initial begin #20000000;$fatal(1,"simulation watchdog");end
endmodule


