// Dedicated optional AXI-Lite slave. Does not change legacy LFN1 address space.
// All service signals are synchronous to clk. One software owner per bank.
module audio_clock_regs #(parameter integer TIMEOUT_CYCLES=250000000)(
 input wire clk,aresetn,
 input wire[11:0]awaddr,input wire awvalid,output wire awready,
 input wire[31:0]wdata,input wire[3:0]wstrb,input wire wvalid,output wire wready,
 output reg[1:0]bresp,output reg bvalid,input wire bready,
 input wire[11:0]araddr,input wire arvalid,output wire arready,
 output reg[31:0]rdata,output reg[1:0]rresp,output reg rvalid,input wire rready,
 output reg command_req,output reg[1:0]command_kind,output reg[31:0]command_seq,
 input wire command_ack,command_error,input wire[31:0]command_ack_seq,
 input wire[3:0]command_reason,input wire service_busy,
 output reg snapshot_req,output reg[31:0]snapshot_request_seq,
 input wire snapshot_ack,snapshot_error,snapshot_valid,
 input wire[31:0]snapshot_ack_seq,input wire[511:0]snapshot_data,
 input wire[63:0]snapshot_age
);
 reg ah,wh;reg[11:0]addr;reg[31:0]data;reg[3:0]strb;
 reg[31:0]cs,ss,ct,st,accepted_snapshot;
 reg[511:0]held_snapshot;reg held_valid;
 wire[31:0]pulse=data & {{8{strb[3]}},{8{strb[2]}},{8{strb[1]}},{8{strb[0]}}};
 assign awready=aresetn&&!ah&&!bvalid;
 assign wready=aresetn&&!wh&&!bvalid;
 assign arready=aresetn&&!rvalid;
 always @(posedge clk or negedge aresetn)begin
  if(!aresetn)begin
   ah<=0;wh<=0;addr<=0;data<=0;strb<=0;bvalid<=0;bresp<=0;
   rvalid<=0;rdata<=0;rresp<=0;command_req<=0;command_kind<=0;command_seq<=0;
   snapshot_req<=0;snapshot_request_seq<=0;cs<=0;ss<=0;ct<=0;st<=0;
   accepted_snapshot<=0;held_snapshot<=0;held_valid<=0;
  end else begin
   if(bvalid&&bready)bvalid<=0;
   if(rvalid&&rready)rvalid<=0;
   if(awvalid&&awready)begin ah<=1;addr<=awaddr;end
   if(wvalid&&wready)begin wh<=1;data<=wdata;strb<=wstrb;end
   if(!snapshot_valid)held_valid<=0;
   if(command_req)begin
    if(ct<TIMEOUT_CYCLES)ct<=ct+1'b1;
    if(ct>=TIMEOUT_CYCLES-1)cs<=cs|8; // timeout retains ownership and tag
    if(command_ack&&command_ack_seq==command_seq)begin
     command_req<=0;cs<={12'd0,command_reason,12'd0,cs[3],command_error,1'b1,1'b0};
    end
   end
   if(snapshot_req)begin
    if(st<TIMEOUT_CYCLES)st<=st+1'b1;
    if(st>=TIMEOUT_CYCLES-1)ss<=ss|8;
    if(snapshot_ack&&snapshot_ack_seq==snapshot_request_seq)begin
     snapshot_req<=0;ss<={28'd0,ss[3],snapshot_error,1'b1,1'b0};
     held_valid<=snapshot_valid&&!snapshot_error;
     if(!snapshot_error)begin held_snapshot<=snapshot_data;accepted_snapshot<=snapshot_ack_seq;end
    end
   end
   if(ah&&wh&&!bvalid)begin
    ah<=0;wh<=0;bvalid<=1;bresp<=0;
    if(addr[1:0]!=0)bresp<=2;
    else case(addr)
     12'h008:if(pulse==0)begin end
      else if((pulse!=1&&pulse!=2)||command_req||service_busy||command_seq==32'hffffffff)bresp<=2;
      else begin command_req<=1;command_kind<=pulse[1:0];command_seq<=command_seq+1'b1;ct<=0;cs<=1;held_valid<=0;end
     12'h020:if(pulse==0)begin end
      else if(pulse!=1||snapshot_req||snapshot_request_seq==32'hffffffff)bresp<=2;
      else begin snapshot_req<=1;snapshot_request_seq<=snapshot_request_seq+1'b1;st<=0;ss<=1;held_valid<=0;end
     default:bresp<=2;
    endcase
   end
   if(arvalid&&arready)begin
    rvalid<=1;rresp<=0;rdata<=0;
    if(araddr[1:0]!=0)rresp<=2;
    else if(araddr>=12'h040&&araddr<12'h080)rdata<=held_snapshot[(araddr-12'h040)*8+:32];
    else case(araddr)
     12'h000:rdata<=32'h31434c41; // ALC1 little endian
     12'h004:rdata<=32'h00010000;
     12'h00c:rdata<=cs;
     12'h010:rdata<=command_seq;
     12'h014:rdata<={31'd0,service_busy};
     12'h024:rdata<=ss|((held_valid&&snapshot_valid)?32'h10:0);
     12'h028:rdata<=accepted_snapshot;
     12'h02c:rdata<=snapshot_age[31:0];
     12'h030:rdata<=snapshot_age[63:32];
     default:rresp<=2;
    endcase
   end
  end
 end
 initial if(TIMEOUT_CYCLES<4)$error("audio register timeout must be >=4");
endmodule
