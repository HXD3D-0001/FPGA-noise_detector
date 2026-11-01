// DAT1: separate optional data/ownership slave. Same clk and system reset as
// CAP1/datapath; one PS owner, one energy consumer. No implicit producer START.
module capture_data_regs #(parameter integer TIMEOUT_CYCLES=5000000)(
 input wire clk,aresetn,
 input wire[11:0]awaddr,input wire awvalid,output wire awready,
 input wire[31:0]wdata,input wire[3:0]wstrb,input wire wvalid,output wire wready,
 output reg[1:0]bresp,output reg bvalid,input wire bready,
 input wire[11:0]araddr,input wire arvalid,output wire arready,
 output reg[31:0]rdata,output reg[1:0]rresp,output reg rvalid,input wire rready,
 input wire capture_start,source_stopped,drained,input wire[31:0]energy_occupancy,
 input wire energy_valid,output wire energy_ready,
 input wire[63:0]energy_seq,energy_capture,energy_first_raw,energy_value,
 input wire[31:0]energy_config,input wire[7:0]energy_flags,
 output wire busy,quarantined,dma_idle,spectrum_output_hold,pcm_output_hold,
 output wire spectrum_flush_req,dma_spectrum_reset_done,dma_pcm_abort,clear_energy_records,
 output wire[31:0]dma_spectrum_reset_seq,
 input wire spectrum_flush_pending,input wire[31:0]spectrum_flush_request_seq
);
 reg ah,wh;reg[11:0]addr;reg[31:0]data;reg[3:0]strb;
 reg command_req;reg[2:0]command_kind;reg[31:0]command_seq,cs;
 wire command_ack,command_error,command_timed_out;wire[31:0]command_ack_seq;wire[3:0]command_reason;
 wire[31:0]dma_request_seq;wire[1:0]dma_request_mask;wire dma_request_reset,dma_request_resume;
 reg dma_ack_valid;reg[31:0]dma_ack_seq,ack_seq_shadow,ack_mask_shadow,ack_status,submitted_ack;reg[1:0]dma_ack_mask;reg ack_seen;
 reg locked,pop;reg[295:0]record_data;reg[63:0]expected_seq;
 wire[295:0]head={energy_seq,energy_capture,energy_first_raw,energy_value,energy_config,energy_flags};
 wire[31:0]pulse=data&{{8{strb[3]}},{8{strb[2]}},{8{strb[1]}},{8{strb[0]}}};
 assign energy_ready=pop&&!clear_energy_records;
 assign awready=aresetn&&!ah&&!bvalid;assign wready=aresetn&&!wh&&!bvalid;assign arready=aresetn&&!rvalid;
 function[31:0]merge;input[31:0]old;integer i;begin merge=old;for(i=0;i<4;i=i+1)if(strb[i])merge[i*8+:8]=data[i*8+:8];end endfunction
 capture_ownership_service #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES)) service(
 .clk(clk),.rst(!aresetn),.capture_start(capture_start),.source_stopped(source_stopped),.drained(drained),.energy_occupancy(energy_occupancy),
 .command_req(command_req),.command_kind(command_kind),.command_seq(command_seq),.command_ack(command_ack),.command_error(command_error),.command_timed_out(command_timed_out),.command_ack_seq(command_ack_seq),.command_reason(command_reason),
 .busy(busy),.quarantined(quarantined),.dma_idle(dma_idle),.dma_request_seq(dma_request_seq),.dma_request_mask(dma_request_mask),.dma_request_reset(dma_request_reset),.dma_request_resume(dma_request_resume),
 .dma_ack_valid(dma_ack_valid),.dma_ack_seq(dma_ack_seq),.dma_ack_mask(dma_ack_mask),.spectrum_flush_req(spectrum_flush_req),.dma_spectrum_reset_done(dma_spectrum_reset_done),.dma_pcm_abort(dma_pcm_abort),.clear_energy_records(clear_energy_records),
 .dma_spectrum_reset_seq(dma_spectrum_reset_seq),.spectrum_flush_pending(spectrum_flush_pending),.spectrum_flush_request_seq(spectrum_flush_request_seq),.spectrum_output_hold(spectrum_output_hold),.pcm_output_hold(pcm_output_hold));
 always@(posedge clk or negedge aresetn)begin
  if(!aresetn)begin
   ah<=0;wh<=0;addr<=0;data<=0;strb<=0;bvalid<=0;bresp<=0;rvalid<=0;rdata<=0;rresp<=0;
   command_req<=0;command_kind<=0;command_seq<=0;cs<=0;dma_ack_valid<=0;dma_ack_seq<=0;dma_ack_mask<=0;
   ack_seq_shadow<=0;ack_mask_shadow<=0;ack_status<=0;submitted_ack<=0;ack_seen<=0;locked<=0;pop<=0;record_data<=0;expected_seq<=0;
  end else begin
   dma_ack_valid<=0;pop<=0;
   if(clear_energy_records)locked<=0;
   if(bvalid&&bready)bvalid<=0;if(rvalid&&rready)rvalid<=0;
   if(awvalid&&awready)begin ah<=1;addr<=awaddr;end
   if(wvalid&&wready)begin wh<=1;data<=wdata;strb<=wstrb;end
   if(command_req&&command_timed_out&&command_ack_seq==command_seq)cs<=cs|8;
   if(command_req&&command_ack&&command_ack_seq==command_seq)begin
    command_req<=0;cs<={12'd0,command_reason,12'd0,(cs[3]||command_timed_out),command_error,1'b1,1'b0};
   end
   if(ah&&wh&&!bvalid)begin
    ah<=0;wh<=0;bvalid<=1;bresp<=0;
    if(addr[1:0]!=0)bresp<=2;
    else case(addr)
     12'h008:if(pulse==0)begin end
      else if(pulse>4||command_req||busy||command_seq==32'hffffffff)bresp<=2;
      else begin command_req<=1;command_kind<=pulse[2:0];command_seq<=command_seq+1'b1;cs<=1;end
     12'h020:ack_seq_shadow<=merge(ack_seq_shadow);
     12'h024:ack_mask_shadow<=merge(ack_mask_shadow);
     12'h028:if(pulse==0)begin end
      else if(pulse!=1||dma_request_mask==0||ack_seq_shadow!=dma_request_seq||ack_mask_shadow!={30'd0,dma_request_mask}||(ack_seen&&submitted_ack==dma_request_seq))begin
       bresp<=2;ack_status<=6;
      end else begin
       dma_ack_valid<=1;dma_ack_seq<=ack_seq_shadow;dma_ack_mask<=ack_mask_shadow[1:0];ack_status<=2;submitted_ack<=dma_request_seq;ack_seen<=1;
      end
     12'h040:if(pulse==0)begin end
      else if(pulse!=1||!energy_valid||clear_energy_records)bresp<=2;
      else begin locked<=1;record_data<=head;end
     12'h044:if(pulse==0)begin end
      else if(pulse!=1||!locked||!energy_valid||clear_energy_records||expected_seq!=record_data[295:232]||head!=record_data)bresp<=2;
      else begin pop<=1;locked<=0;end
     12'h048:expected_seq[31:0]<=merge(expected_seq[31:0]);
     12'h04c:expected_seq[63:32]<=merge(expected_seq[63:32]);
     default:bresp<=2;
    endcase
   end
   if(arvalid&&arready)begin
    rvalid<=1;rresp<=0;rdata<=0;
    if(araddr[1:0]!=0)rresp<=2;
    else case(araddr)
     12'h000:rdata<=32'h31544144;12'h004:rdata<=32'h00010000;
     12'h00c:rdata<=cs;12'h010:rdata<=command_seq;
     12'h014:rdata<={29'd0,dma_idle,quarantined,busy};
     12'h018:rdata<=dma_request_seq;
     12'h01c:rdata<={22'd0,dma_request_resume,dma_request_reset,6'd0,dma_request_mask};
     12'h02c:rdata<=ack_status;
     12'h050:rdata<=record_data[263:232];12'h054:rdata<=record_data[295:264];
     12'h058:rdata<=record_data[199:168];12'h05c:rdata<=record_data[231:200];
     12'h060:rdata<=record_data[135:104];12'h064:rdata<=record_data[167:136];
     12'h068:rdata<=record_data[71:40];12'h06c:rdata<=record_data[103:72];
     12'h070:rdata<=record_data[39:8];12'h074:rdata<={24'd0,record_data[7:0]};
     12'h078:rdata<={31'd0,locked&&!clear_energy_records};12'h07c:rdata<=energy_occupancy;
     default:rresp<=2;
    endcase
   end
  end
 end
endmodule
