// Immutable bank -> protocol v1 packet. The bank is read twice: CRC, then AXIS.
// Ownership lasts until packet_done. abort requires coordinated DMA reset.
module axis_packetizer(
 input wire clk,rst,abort,packet_valid, output wire packet_ready,
 input wire [15:0] packet_type,packet_flags,
 input wire [63:0] packet_capture_id,packet_seq,packet_first_raw,
 input wire [31:0] packet_config_id, input wire [7:0] packet_blk_exp,
 output reg packet_done,packet_error,
 output wire read_en, output wire [12:0] read_addr,
 input wire read_valid, input wire [31:0] read_data,
 output wire m_axis_tvalid, input wire m_axis_tready,
 output wire [63:0] m_axis_tdata, output wire [7:0] m_axis_tkeep,
 output wire m_axis_tlast, output reg [63:0] error_count,completed_count
);
 localparam IDLE=0,HCRC=1,CREQ=2,CWAIT=3,HOUT=4,
            LREQ=5,LWAIT=6,RREQ=7,RWAIT=8,POUT=9;
 reg [3:0] state;
 reg [511:0] header;
 reg [31:0] crc;
 reg [5:0] header_byte;
 reg [2:0] header_word;
 reg [13:0] value_count;
 reg [12:0] index;
 reg [63:0] payload_word;
 wire [13:0] descriptor_values=(packet_type==2)?14'd256:14'd8192;
 wire [31:0] descriptor_sources=(packet_type==1)?32'd4096:
                                     ((packet_type==2)?32'd256:32'd8192);
 wire [31:0] descriptor_stride=(packet_type==3)?32'd1:32'd48;
 // These are protocol geometries, not a variable multiply. Keep the full
 // unsigned anchor-overflow check without inferring a datapath multiplier.
 wire [63:0] descriptor_span=(packet_type==1)?64'd196608:
                            ((packet_type==2)?64'd12288:64'd8192);
 wire descriptor_ok=(packet_type>=1 && packet_type<=3) &&
   packet_capture_id!=0 && packet_config_id!=0 && packet_flags<=16'd63 &&
   ((packet_type==1 && packet_blk_exp<=12) ||
    (packet_type!=1 && packet_blk_exp==0)) &&
   packet_first_raw<=64'hffffffffffffffff-descriptor_span;
 function [31:0] crc_byte;
  input [31:0] c; input [7:0] b; reg [31:0] x; integer k;
  begin x=c^{24'b0,b};for(k=0;k<8;k=k+1)
    x=x[0]?(x>>1)^32'h82f63b78:(x>>1);crc_byte=x;end
 endfunction
 function [31:0] crc_word;
  input [31:0] c,w;reg [31:0] x;integer k;
  begin x=c;for(k=0;k<4;k=k+1)x=crc_byte(x,w[k*8+:8]);crc_word=x;end
 endfunction
 assign packet_ready=(state==IDLE)&&!rst&&!abort;
 assign read_en=!rst&&!abort&&(state==CREQ||state==LREQ||state==RREQ);
 assign read_addr=index;
 assign m_axis_tvalid=!rst&&!abort&&(state==HOUT||state==POUT);
 assign m_axis_tdata=(state==HOUT)?header[header_word*64+:64]:payload_word;
 assign m_axis_tkeep=8'hff;
 assign m_axis_tlast=state==POUT && {1'b0,index}==value_count-1;
 always @(posedge clk) begin
  if(rst)begin
   state<=IDLE;header<=0;crc<=0;header_byte<=0;header_word<=0;
   value_count<=0;index<=0;payload_word<=0;
   packet_done<=0;packet_error<=0;error_count<=0;completed_count<=0;
  end else begin
   packet_done<=0;packet_error<=0;
   if(abort)begin
    if(state!=IDLE)begin packet_done<=1;packet_error<=1;error_count<=error_count+1;end
    state<=IDLE;
   end else case(state)
    IDLE:if(packet_valid)begin
     if(!descriptor_ok)begin
      packet_done<=1;packet_error<=1;error_count<=error_count+1;
     end else begin
      // Byte offsets are the shared/protocol.h wire contract, little endian.
      header<=0;header[31:0]<=32'h314e464c;header[47:32]<=16'd1;
      header[63:48]<=packet_type;header[79:64]<=16'd64;
      header[95:80]<=packet_flags;header[127:96]<={16'b0,descriptor_values,2'b0};
      header[191:128]<=packet_capture_id;header[255:192]<=packet_seq;
      header[319:256]<=packet_first_raw;header[351:320]<=descriptor_sources;
      header[383:352]<=descriptor_stride;header[415:384]<=packet_config_id;
      header[447:416]<=32'd48000;header[463:448]<=16'd24;
      header[471:464]<=packet_blk_exp;header[479:472]<=(packet_type==1)?8'd2:8'd1;
      crc<=32'hffffffff;header_byte<=0;header_word<=0;index<=0;
      value_count<=descriptor_values;state<=HCRC;
     end
    end
    HCRC:begin
     crc<=crc_byte(crc,header[header_byte*8+:8]);
     if(header_byte==59)state<=CREQ;else header_byte<=header_byte+1;
    end
    CREQ:state<=CWAIT;
    CWAIT:if(read_valid)begin
     if(read_data[31:24]!={8{read_data[23]}})begin
      packet_done<=1;packet_error<=1;error_count<=error_count+1;state<=IDLE;
     end else begin
      crc<=crc_word(crc,read_data);
      if({1'b0,index}==value_count-1)begin
       header[511:480]<=crc_word(crc,read_data)^32'hffffffff;
       index<=0;state<=HOUT;
      end else begin index<=index+1;state<=CREQ;end
     end
    end
    HOUT:if(m_axis_tready)begin
     if(header_word==7)state<=LREQ;else header_word<=header_word+1;
    end
    LREQ:state<=LWAIT;
    LWAIT:if(read_valid)begin payload_word[31:0]<=read_data;index<=index+1;state<=RREQ;end
    RREQ:state<=RWAIT;
    RWAIT:if(read_valid)begin payload_word[63:32]<=read_data;state<=POUT;end
    POUT:if(m_axis_tready)begin
     if({1'b0,index}==value_count-1)begin
      packet_done<=1;completed_count<=completed_count+1;state<=IDLE;
     end else begin index<=index+1;state<=LREQ;end
    end
    default:state<=IDLE;
   endcase
  end
 end
endmodule
