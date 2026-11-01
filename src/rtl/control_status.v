// AXI-Lite control, all service inputs synchronous to clk. Completion ACKs must
// echo request_seq; timeout cancels a request and old ACKs cannot apply a new one.
module control_status #(
 parameter CONTROL_TIMEOUT_TICKS=5000000, // 100 ms at 50 MHz
 parameter SNAPSHOT_TIMEOUT_TICKS=50000,
 parameter SNAPSHOT_STALE_TICKS=50000000
)(
 input wire clk,aresetn,
 input wire [11:0] awaddr,input wire awvalid,output wire awready,
 input wire [31:0] wdata,input wire [3:0] wstrb,input wire wvalid,output wire wready,
 output reg [1:0] bresp,output reg bvalid,input wire bready,
 input wire [11:0] araddr,input wire arvalid,output wire arready,
 output reg [31:0] rdata,output reg [1:0] rresp,output reg rvalid,input wire rready,
 input wire [31:0] state_i,status_i,irq_events_i,output wire irq,
 output reg command_req,output reg [2:0] command_kind,output reg [31:0] command_seq,
 input wire command_ack,command_error,input wire [31:0] command_ack_seq,
 output reg commit_req,output reg [31:0] commit_mode,commit_config_id,commit_seq,
 input wire commit_ack,commit_error,input wire [31:0] commit_ack_seq,
 output reg [31:0] mode_active,config_id_active,
 output reg snapshot_req,output reg [31:0] snapshot_request_seq,
 input wire snapshot_ack,snapshot_error,snapshot_invalidate,input wire [31:0] snapshot_ack_seq,
 // Counters: capture/raw/rx_drop/saturation/fft_drop/pcm_drop/protocol/clock,
 // adc_count/adc_clip/fir0/fir1/fir2/energy_drop/raw_drop/energy_highwater.
 input wire [1023:0] snapshot_counters_i,
 input wire[63:0]snapshot_energy_flush_i,
 input wire[63:0]snapshot_pcm_flush_i,
 input wire[255:0]snapshot_sequences_i,
 input wire [31:0] snapshot_source_id_i,input wire snapshot_position_valid_i,
 input wire [63:0] snapshot_latest_raw_i,snapshot_origin_raw_i,
 output reg recovery_req,input wire recovery_ack,
 input wire energy_valid,energy_invalidate,output wire energy_ready,input wire [31:0] energy_count_i,
 input wire [63:0] energy_seq,energy_capture_id,energy_first_raw,energy_value,
 input wire [31:0] energy_config_id,input wire [7:0] energy_flags,
 input wire[31:0]dma_reset_request_seq_i,input wire[1:0]dma_reset_request_mask_i,input wire dma_reset_request_reset_i,
 output reg dma_reset_ack_valid,output reg[31:0]dma_reset_ack_seq,output reg[1:0]dma_reset_ack_mask
 ,output reg flow_req,output reg[1:0]flow_mask,output reg[31:0]flow_seq,
 input wire flow_ack,flow_error,input wire[31:0]flow_ack_seq
);
 reg awheld,wheld;reg [11:0] wa;reg [31:0] wd;reg [3:0] ws;
 reg [31:0] mode_shadow,config_shadow,irq_mask,irq_status;
 reg [31:0] command_status,commit_status,snapshot_status,snapshot_seq;
 reg [31:0] command_ticks,commit_ticks,snapshot_ticks;
 reg [1023:0] counters;reg [31:0] source_id;reg position_valid;
 reg [63:0] latest_raw,origin_raw,snapshot_age;
 reg latch_valid;reg [63:0] latch_seq,latch_capture,latch_first,latch_energy,expected_seq;
 reg [31:0] latch_config;reg [7:0] latch_flags;
 reg[31:0]dma_ack_seq_shadow,dma_ack_mask_shadow,dma_ack_status,dma_last_ack_seq;
 reg dma_last_ack_valid;reg[63:0]snapshot_energy_flush;reg[255:0]snapshot_sequences;reg[63:0]snapshot_pcm_flush;
 reg[31:0]flow_mask_shadow,flow_status,flow_ticks;
 wire write_fire=awheld&&wheld&&!bvalid;
 wire [31:0] pulse=wd&{{8{ws[3]}},{8{ws[2]}},{8{ws[1]}},{8{ws[0]}}};
 wire pop_match=latch_valid&&energy_valid&&expected_seq==latch_seq&&energy_seq==latch_seq;
 assign energy_ready=aresetn&&!energy_invalidate&&write_fire&&wa==12'h084&&pulse==32'd1&&pop_match;
 assign awready=aresetn&&!awheld&&!bvalid;
 assign wready=aresetn&&!wheld&&!bvalid;
 assign arready=aresetn&&!rvalid;
 assign irq=|(irq_status&irq_mask);
 function [31:0] merged(input [31:0] old,input [31:0] data,input [3:0] strobe);
 integer j;begin merged=old;for(j=0;j<4;j=j+1)if(strobe[j])merged[j*8+:8]=data[j*8+:8];end endfunction
 function [31:0] read_reg(input [11:0] a);
 begin
 // Fixed ABI slices avoid wide variable shifts and index arithmetic.
 case(a)
 'h040:read_reg=counters[31:0];
 'h044:read_reg=counters[63:32];
 'h048:read_reg=counters[95:64];
 'h04c:read_reg=counters[127:96];
 'h050:read_reg=counters[159:128];
 'h054:read_reg=counters[191:160];
 'h058:read_reg=counters[223:192];
 'h05c:read_reg=counters[255:224];
 'h060:read_reg=counters[287:256];
 'h064:read_reg=counters[319:288];
 'h068:read_reg=counters[351:320];
 'h06c:read_reg=counters[383:352];
 'h070:read_reg=counters[415:384];
 'h074:read_reg=counters[447:416];
 'h078:read_reg=counters[479:448];
 'h07c:read_reg=counters[511:480];
 'h100:read_reg=counters[607:576];
 'h104:read_reg=counters[639:608];
 'h108:read_reg=counters[671:640];
 'h10c:read_reg=counters[703:672];
 'h110:read_reg=counters[735:704];
 'h114:read_reg=counters[767:736];
 'h118:read_reg=counters[799:768];
 'h11c:read_reg=counters[831:800];
 'h120:read_reg=counters[863:832];
 'h124:read_reg=counters[895:864];
 'h128:read_reg=counters[927:896];
 'h12c:read_reg=counters[959:928];
 'h130:read_reg=counters[991:960];
 'h134:read_reg=counters[1023:992];
 'h158:read_reg=snapshot_sequences[31:0];
 'h15c:read_reg=snapshot_sequences[63:32];
 'h160:read_reg=snapshot_sequences[95:64];
 'h164:read_reg=snapshot_sequences[127:96];
 'h168:read_reg=snapshot_sequences[159:128];
 'h16c:read_reg=snapshot_sequences[191:160];
 'h170:read_reg=snapshot_sequences[223:192];
 'h174:read_reg=snapshot_sequences[255:224];
 'h000:read_reg=32'h314e464c;'h004:read_reg=1;
 'h00c:read_reg=mode_shadow;'h010:read_reg=config_shadow;
 'h018:read_reg=state_i;'h01c:read_reg=status_i;
 'h020:read_reg=irq_mask;'h024:read_reg=irq_status;
 'h030:read_reg=snapshot_seq;'h034:read_reg=energy_count_i;
 'h088:read_reg=latch_seq[31:0];'h08c:read_reg=latch_seq[63:32];
 'h090:read_reg=latch_capture[31:0];'h094:read_reg=latch_capture[63:32];
 'h098:read_reg=latch_first[31:0];'h09c:read_reg=latch_first[63:32];
 'h0a0:read_reg=latch_energy[31:0];'h0a4:read_reg=latch_energy[63:32];
 'h0a8:read_reg=latch_config;'h0ac:read_reg={24'b0,latch_flags};'h0b0:read_reg={31'b0,latch_valid};
 'h0bc:read_reg=expected_seq[31:0];'h0c0:read_reg=expected_seq[63:32];
 'h0c4:read_reg=mode_active;'h0c8:read_reg=config_id_active;'h0cc:read_reg=commit_status;
 'h0d0:read_reg=snapshot_status|((snapshot_status[4]&&snapshot_age>=SNAPSHOT_STALE_TICKS)?32'h20:0);
 'h0d4:read_reg=snapshot_age[31:0];'h0d8:read_reg=snapshot_age[63:32];'h0dc:read_reg=command_status;
 'h0e0:read_reg=source_id;'h0e4:read_reg={31'b0,position_valid};
 'h0e8:read_reg=latest_raw[31:0];'h0ec:read_reg=latest_raw[63:32];
 'h0f0:read_reg=origin_raw[31:0];'h0f4:read_reg=origin_raw[63:32];
 'h0f8:read_reg=counters[543:512];'h0fc:read_reg=counters[575:544];
 'h138:read_reg=dma_reset_request_seq_i;'h13c:read_reg={23'b0,dma_reset_request_reset_i,6'b0,dma_reset_request_mask_i};
 'h140:read_reg=dma_ack_seq_shadow;'h144:read_reg=dma_ack_mask_shadow;'h14c:read_reg=dma_ack_status;
 'h180:read_reg=flow_mask_shadow;'h188:read_reg=flow_status;'h18c:read_reg=flow_seq;
 'h178:read_reg=snapshot_pcm_flush[31:0];'h17c:read_reg=snapshot_pcm_flush[63:32];
 'h150:read_reg=snapshot_energy_flush[31:0];'h154:read_reg=snapshot_energy_flush[63:32];
 default:read_reg=0;
 endcase
 end endfunction
 always @(posedge clk)begin
 if(!aresetn)begin
 awheld<=0;wheld<=0;wa<=0;wd<=0;ws<=0;bvalid<=0;bresp<=0;rvalid<=0;rresp<=0;rdata<=0;
 mode_shadow<=0;config_shadow<=0;mode_active<=0;config_id_active<=0;
 irq_mask<=0;irq_status<=0;command_req<=0;command_kind<=0;command_seq<=0;command_status<=0;command_ticks<=0;
 commit_req<=0;commit_seq<=0;commit_mode<=0;commit_config_id<=0;commit_status<=0;commit_ticks<=0;
 snapshot_req<=0;snapshot_request_seq<=0;snapshot_seq<=0;snapshot_status<=0;snapshot_ticks<=0;
 counters<=0;source_id<=0;position_valid<=0;latest_raw<=0;origin_raw<=0;snapshot_age<=0;
 recovery_req<=0;latch_valid<=0;latch_seq<=0;latch_capture<=0;latch_first<=0;latch_energy<=0;latch_config<=0;latch_flags<=0;expected_seq<=0;
 dma_reset_ack_valid<=0;dma_reset_ack_seq<=0;dma_reset_ack_mask<=0;dma_ack_seq_shadow<=0;dma_ack_mask_shadow<=0;dma_ack_status<=0;dma_last_ack_seq<=0;dma_last_ack_valid<=0;snapshot_energy_flush<=0;snapshot_pcm_flush<=0;snapshot_sequences<=0;
 flow_req<=0;flow_mask<=0;flow_seq<=0;flow_mask_shadow<=0;flow_status<=0;flow_ticks<=0;
 end else begin
 irq_status<=irq_status|irq_events_i;
 dma_reset_ack_valid<=0;
 if(recovery_ack)recovery_req<=0;
 if(flow_req)begin
  if(flow_ack&&flow_ack_seq==flow_seq)begin flow_req<=0;flow_status<=(flow_status&8)|(flow_error?32'h50004:2);end
  else if(flow_ticks>=CONTROL_TIMEOUT_TICKS-1)begin flow_status<=32'h60009;end
  else flow_ticks<=flow_ticks+1'b1;
 end
 if(snapshot_status[4]&&snapshot_age!=64'hffffffffffffffff)snapshot_age<=snapshot_age+1'b1;
 if(awvalid&&awready)begin wa<=awaddr;awheld<=1;end
 if(wvalid&&wready)begin wd<=wdata;ws<=wstrb;wheld<=1;end
 if(bvalid&&bready)bvalid<=0;
 if(rvalid&&rready)rvalid<=0;
 if(arvalid&&arready)begin rvalid<=1;rresp<=araddr[1:0]!=0?2'b10:2'b00;rdata<=araddr[1:0]!=0?0:read_reg(araddr);end
 if(command_req)begin
 if(command_ack&&command_ack_seq==command_seq)begin command_req<=0;command_status<=command_error?32'h50004:2;end
 else if(command_ticks>=CONTROL_TIMEOUT_TICKS-1)begin command_req<=0;command_status<=32'h60008;recovery_req<=1;end
 else command_ticks<=command_ticks+1'b1;
 end
 if(commit_req)begin
 if(commit_ack&&commit_ack_seq==commit_seq)begin
 commit_req<=0;commit_status<=commit_error?32'h50004:2;
 if(!commit_error)begin mode_active<=commit_mode;config_id_active<=commit_config_id;snapshot_status<=0;snapshot_req<=0;latch_valid<=0;end
 end else if(commit_ticks>=CONTROL_TIMEOUT_TICKS-1)begin commit_req<=0;commit_status<=32'h60008;recovery_req<=1;end
 else commit_ticks<=commit_ticks+1'b1;
 end
 if(snapshot_req)begin
 if(snapshot_ack&&snapshot_ack_seq==snapshot_request_seq)begin
 snapshot_req<=0;
 if(snapshot_error)snapshot_status<=(snapshot_status&32'h10)|32'h50004;
 else begin snapshot_status<=32'h12;snapshot_seq<=snapshot_seq+1'b1;snapshot_age<=0;
 counters<=snapshot_counters_i;snapshot_energy_flush<=snapshot_energy_flush_i;snapshot_pcm_flush<=snapshot_pcm_flush_i;snapshot_sequences<=snapshot_sequences_i;source_id<=snapshot_source_id_i;position_valid<=snapshot_position_valid_i;latest_raw<=snapshot_latest_raw_i;origin_raw<=snapshot_origin_raw_i;end
 end else if(snapshot_ticks>=SNAPSHOT_TIMEOUT_TICKS-1)begin snapshot_req<=0;snapshot_status<=(snapshot_status&32'h10)|32'h60008;end
 else snapshot_ticks<=snapshot_ticks+1'b1;
 end
 if(write_fire)begin
 awheld<=0;wheld<=0;bvalid<=1;bresp<=0;
 if(wa[1:0]!=0)bresp<=2;
 else case(wa)
 'h008:begin
 if(|pulse[31:4]||((pulse[2:0]!=0)&&(pulse[2:0]!=1)&&(pulse[2:0]!=2)&&(pulse[2:0]!=4)))begin bresp<=2;command_status<=(command_status&32'hffff)|32'h30004;end
 else if(pulse[2:0]!=0)begin
 if(command_req||commit_req||recovery_req||flow_req||(pulse[0]&&state_i!=0)||command_seq==32'hffffffff)begin bresp<=2;command_status<=(command_status&32'hffff)|((command_seq==32'hffffffff)?32'h70004:((pulse[0]&&state_i!=0)?32'h20004:32'h10004));end
 else begin command_req<=1;command_kind<=pulse[0]?1:(pulse[1]?2:3);command_seq<=command_seq+1'b1;command_status<=1;command_ticks<=0;
 if(pulse[2])begin latch_valid<=0;snapshot_status<=0;snapshot_req<=0;end
 end
 end
 if(pulse[3]&&!(|pulse[31:4])&&((pulse[2:0]==0)||(pulse[2:0]==1)||(pulse[2:0]==2)||(pulse[2:0]==4)))irq_status<=irq_events_i;
 end
 'h00c:if(commit_req||command_req||recovery_req||flow_req) bresp<=2;else mode_shadow<=merged(mode_shadow,wd,ws);
 'h010:if(commit_req||command_req||recovery_req||flow_req)bresp<=2;else config_shadow<=merged(config_shadow,wd,ws);
 'h014:begin
 if(|pulse[31:1])begin bresp<=2;commit_status<=(commit_status&32'hffff)|32'h30004;end
 else if(pulse[0])begin
 if(state_i!=0||command_req||commit_req||recovery_req||flow_req||mode_shadow>3||config_shadow==0||commit_seq==32'hffffffff)begin bresp<=2;commit_status<=(commit_status&32'hffff)|((commit_seq==32'hffffffff)?32'h70004:((state_i!=0)?32'h20004:((mode_shadow>3||config_shadow==0)?32'h40004:32'h10004)));end
 else begin commit_req<=1;commit_seq<=commit_seq+1'b1;commit_mode<=mode_shadow;commit_config_id<=config_shadow;commit_status<=1;commit_ticks<=0;end
 end end
 'h020:irq_mask<=merged(irq_mask,wd,ws);
 'h028:irq_status<=(irq_status&~pulse)|irq_events_i;
 'h02c:begin
 if(|pulse[31:1])bresp<=2;
 else if(pulse[0])begin
 if(snapshot_req||snapshot_request_seq==32'hffffffff||snapshot_seq==32'hffffffff)begin bresp<=2;snapshot_status<=(snapshot_status&32'hffff)|((snapshot_req)?32'h10004:32'h70004);end
 else begin snapshot_req<=1;snapshot_request_seq<=snapshot_request_seq+1'b1;snapshot_status<=(snapshot_status&32'h10)|1;snapshot_ticks<=0;end
 end end
 'h080:begin
 if(|pulse[31:1])bresp<=2;
 else if(pulse[0])begin
 // A second LATCH may re-read the same head, but cannot replace an outstanding lock.
 if(latch_valid) bresp<=2;
 else if(!energy_valid)begin latch_valid<=0;bresp<=2;end
 else begin latch_valid<=1;latch_seq<=energy_seq;latch_capture<=energy_capture_id;latch_first<=energy_first_raw;latch_energy<=energy_value;latch_config<=energy_config_id;latch_flags<=energy_flags;end
 end end
 'h084:begin
 if(|pulse[31:1])bresp<=2;
 else if(pulse[0])begin if(pop_match&&!energy_invalidate)latch_valid<=0;else bresp<=2;end
 end
 'h0bc:expected_seq[31:0]<=merged(expected_seq[31:0],wd,ws);
 'h0c0:expected_seq[63:32]<=merged(expected_seq[63:32],wd,ws);
 'h140:dma_ack_seq_shadow<=merged(dma_ack_seq_shadow,wd,ws);
 'h144:dma_ack_mask_shadow<=merged(dma_ack_mask_shadow,wd,ws);
 'h148:begin
 if(|pulse[31:1])begin bresp<=2;dma_ack_status<=32'h30004;end
 else if(pulse[0])begin
 if(dma_reset_request_mask_i==0||dma_ack_seq_shadow!=dma_reset_request_seq_i||dma_ack_mask_shadow!={30'b0,dma_reset_request_mask_i}||(dma_last_ack_valid&&dma_last_ack_seq==dma_ack_seq_shadow))begin bresp<=2;dma_ack_status<=32'h80004;end
 else begin dma_reset_ack_valid<=1;dma_reset_ack_seq<=dma_ack_seq_shadow;dma_reset_ack_mask<=dma_ack_mask_shadow[1:0];dma_ack_status<=2;dma_last_ack_seq<=dma_ack_seq_shadow;dma_last_ack_valid<=1;end
 end end
 'h180:if(flow_req||command_req||commit_req||recovery_req)bresp<=2;else flow_mask_shadow<=merged(flow_mask_shadow,wd,ws);
 'h184:begin
 if(|pulse[31:1])begin bresp<=2;flow_status<=(flow_status&9)|32'h30004;end
 else if(pulse[0])begin
 if(flow_req||command_req||commit_req||recovery_req||(state_i!=4&&state_i!=5)||
  (flow_mask_shadow!=1&&flow_mask_shadow!=2)||flow_seq==32'hffffffff)begin
  bresp<=2;flow_status<=(flow_status&9)|32'h20004;
 end else begin flow_req<=1;flow_mask<=flow_mask_shadow[1:0];flow_seq<=flow_seq+1'b1;flow_status<=1;flow_ticks<=0;end
 end end
 default:bresp<=2;
 endcase
 end
 // Source/capture identity changes cancel in-flight snapshot and invalidate the
 // old image. Highest precedence prevents a same-cycle old ACK publishing it.
 if(snapshot_invalidate||(commit_req&&commit_ack&&commit_ack_seq==commit_seq&&!commit_error))begin snapshot_req<=0;snapshot_status<=0;end
 if(energy_invalidate)latch_valid<=0;
 end
 end
endmodule
