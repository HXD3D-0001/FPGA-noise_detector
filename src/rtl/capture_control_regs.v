// CAP1: optional single-owner control plane. Replaces (never parallels) the
// LFN1 capture owner / ALC1 command owner on a connected datapath.
// All boundary signals share clk; stop_complete includes real DMA ownership.
module capture_control_regs #(
 parameter integer TIMEOUT_CYCLES=250000000,parameter[63:0]BOOT_ORIGIN=0
)(
 input wire clk,aresetn,
 input wire[11:0]awaddr,input wire awvalid,output wire awready,
 input wire[31:0]wdata,input wire[3:0]wstrb,input wire wvalid,output wire wready,
 output reg[1:0]bresp,output reg bvalid,input wire bready,
 input wire[11:0]araddr,input wire arvalid,output wire arready,
 output reg[31:0]rdata,output reg[1:0]rresp,output reg rvalid,input wire rready,
 input wire stop_complete,start_ready,running,command_rejected,
 input wire mclk_ready_proc,clock_fault_latched,clock_restart_pending,
 input wire[3:0]frontend_state,fault_reason,
 input wire[63:0]active_capture_id,input wire[31:0]active_config_id,
 input wire active_test_mode,mode_ready,mode_rejected,input wire[1:0]mode_active,
 input wire[63:0]adc_count,test_count,latest_raw,input wire source_position_valid,
 output wire mode_valid,output wire[1:0]mode_select,
 output wire start_req,stop_req,requested_test_mode,channel_select,
 output wire[1:0]pattern_id,output wire[31:0]requested_config_id,
 output wire[63:0]next_capture_id,origin_raw,
 output wire armed,service_busy,quarantined,transaction_busy,
 output reg clock_command_req,output reg[1:0]clock_command_kind,
 output wire[31:0]clock_command_seq,
 input wire clock_command_ack,clock_command_error,input wire[31:0]clock_command_ack_seq,
 input wire[3:0]clock_command_reason,input wire clock_control_busy
);
 reg ah,wh;reg[11:0]addr;reg[31:0]data;reg[3:0]strb;
 reg[31:0]shadow_mode,shadow_source,shadow_channel,shadow_pattern,shadow_config;
 reg[31:0]configured_mode,configured_source,configured_channel,configured_pattern,configured_config;
 reg commit_req,command_req,command_pending;reg[1:0]command_kind;
 reg[31:0]commit_seq,command_seq,commit_mode,commit_source,commit_channel,commit_pattern,commit_config_id;
 wire commit_ack,commit_error,command_ack,command_error,commit_timed_out,command_timed_out;
 wire[31:0]commit_ack_seq,command_ack_seq;wire[3:0]commit_reason,command_reason,last_fault_reason;
 wire[2:0]operation;
 reg[31:0]cs,ms,ct,mt,snapshot_seq;reg[575:0]snapshot_data;
 reg snapshot_valid;reg[63:0]snapshot_age;reg[319:0]snapshot_key;
 wire occupied=command_pending||commit_req||service_busy||clock_control_busy;
 assign transaction_busy=occupied;
 wire[31:0]flags={2'd0,mode_active,last_fault_reason,fault_reason,frontend_state,
  4'd0,operation,clock_control_busy,source_position_valid,active_test_mode,start_ready,stop_complete,running,quarantined,service_busy,armed};
 wire[319:0]key={flags,commit_seq,active_capture_id,active_config_id,
  configured_mode,configured_source,configured_channel,configured_pattern,configured_config};
 wire[31:0]pulse=data & {{8{strb[3]}},{8{strb[2]}},{8{strb[1]}},{8{strb[0]}}};
 wire selected_ack=clock_command_req?clock_command_ack:command_ack;
 wire selected_error=clock_command_req?clock_command_error:command_error;
 wire[31:0]selected_tag=clock_command_req?clock_command_ack_seq:command_ack_seq;
 wire[3:0]selected_reason=clock_command_req?clock_command_reason:command_reason;
 wire selected_timeout=clock_command_req?(clock_command_error&&clock_command_reason==3):command_timed_out;
 assign clock_command_seq=command_seq;
 assign awready=aresetn&&!ah&&!bvalid;assign wready=aresetn&&!wh&&!bvalid;assign arready=aresetn&&!rvalid;
 function[31:0]merge;input[31:0]old;integer i;begin
  merge=old;for(i=0;i<4;i=i+1)if(strb[i])merge[i*8+:8]=data[i*8+:8];
 end endfunction
 capture_control_service #(.TIMEOUT_CYCLES(TIMEOUT_CYCLES),.BOOT_ORIGIN(BOOT_ORIGIN)) service(
  .clk(clk),.rst(!aresetn),.commit_req(commit_req),.commit_seq(commit_seq),.commit_mode(commit_mode),.commit_source(commit_source),
  .commit_channel(commit_channel),.commit_pattern(commit_pattern),.commit_config_id(commit_config_id),
  .commit_ack(commit_ack),.commit_error(commit_error),.commit_ack_seq(commit_ack_seq),.commit_reason(commit_reason),.commit_timed_out(commit_timed_out),
  .command_req(command_req),.command_kind(command_kind),.command_seq(command_seq),.command_ack(command_ack),.command_error(command_error),
  .command_ack_seq(command_ack_seq),.command_reason(command_reason),.command_timed_out(command_timed_out),
  .stop_complete(stop_complete),.start_ready(start_ready),.running(running),.command_rejected(command_rejected),
  .clock_control_busy(clock_control_busy||clock_command_req),.frontend_state(frontend_state),.fault_reason(fault_reason),
  .active_capture_id(active_capture_id),.active_config_id(active_config_id),.active_test_mode(active_test_mode),.mode_ready(mode_ready),.mode_rejected(mode_rejected),.mode_active(mode_active),
  .mode_valid(mode_valid),.mode_select(mode_select),.start_req(start_req),.stop_req(stop_req),.requested_test_mode(requested_test_mode),
  .channel_select(channel_select),.pattern_id(pattern_id),.requested_config_id(requested_config_id),.next_capture_id(next_capture_id),.origin_raw(origin_raw),
  .armed(armed),.busy(service_busy),.quarantined(quarantined),.operation(operation),.last_fault_reason(last_fault_reason));
 always@(posedge clk or negedge aresetn)begin
  if(!aresetn)begin
   ah<=0;wh<=0;addr<=0;data<=0;strb<=0;bvalid<=0;bresp<=0;rvalid<=0;rdata<=0;rresp<=0;
   shadow_mode<=0;shadow_source<=0;shadow_channel<=0;shadow_pattern<=0;shadow_config<=0;
   configured_mode<=0;configured_source<=0;configured_channel<=0;configured_pattern<=0;configured_config<=0;
   commit_req<=0;command_req<=0;command_pending<=0;command_kind<=0;commit_seq<=0;command_seq<=0;
   commit_mode<=0;commit_source<=0;commit_channel<=0;commit_pattern<=0;commit_config_id<=0;
   clock_command_req<=0;clock_command_kind<=0;cs<=0;ms<=0;ct<=0;mt<=0;
   snapshot_seq<=0;snapshot_data<=0;snapshot_valid<=0;snapshot_age<=64'hffffffffffffffff;snapshot_key<=0;
  end else begin
   if(bvalid&&bready)bvalid<=0;if(rvalid&&rready)rvalid<=0;
   if(awvalid&&awready)begin ah<=1;addr<=awaddr;end
   if(wvalid&&wready)begin wh<=1;data<=wdata;strb<=wstrb;end
   if(snapshot_valid&&snapshot_age!=64'hffffffffffffffff)snapshot_age<=snapshot_age+1'b1;
   if(snapshot_key!=key)snapshot_valid<=0;
   if(command_pending)begin
    if(ct<TIMEOUT_CYCLES)ct<=ct+1'b1;
    if(ct>=TIMEOUT_CYCLES-1)cs<=cs|8;
    if(selected_ack&&selected_tag==command_seq)begin
     command_pending<=0;command_req<=0;clock_command_req<=0;
     cs<={12'd0,selected_reason,12'd0,(cs[3]||selected_timeout),selected_error,1'b1,1'b0};
    end
   end
   if(commit_req)begin
    if(mt<TIMEOUT_CYCLES)mt<=mt+1'b1;
    if(mt>=TIMEOUT_CYCLES-1)ms<=ms|8;
    if(commit_ack&&commit_ack_seq==commit_seq)begin
     commit_req<=0;ms<={12'd0,commit_reason,12'd0,(ms[3]||commit_timed_out),commit_error,1'b1,1'b0};
     if(!commit_error)begin
      configured_mode<=commit_mode;configured_source<=commit_source;configured_channel<=commit_channel;
      configured_pattern<=commit_pattern;configured_config<=commit_config_id;
     end
    end
   end
   if(ah&&wh&&!bvalid)begin
    ah<=0;wh<=0;bvalid<=1;bresp<=0;
    if(addr[1:0]!=0)bresp<=2;
    else case(addr)
     12'h008:if(pulse==0)begin end
      else if(pulse>4||occupied||command_seq==32'hffffffff||(pulse>=3&&!stop_complete))begin
       bresp<=2;cs<={24'd0,4'd2,4'd1};
      end
      else begin
       command_seq<=command_seq+1'b1;command_pending<=1;ct<=0;cs<=1;snapshot_valid<=0;
       command_req<=(pulse<=2);command_kind<=pulse[1:0];
       clock_command_req<=(pulse>=3);clock_command_kind<=pulse==3?1:2;
      end
     12'h020:shadow_mode<=merge(shadow_mode);
     12'h024:shadow_source<=merge(shadow_source);
     12'h028:shadow_channel<=merge(shadow_channel);
     12'h02c:shadow_pattern<=merge(shadow_pattern);
     12'h030:shadow_config<=merge(shadow_config);
     12'h034:if(pulse==0)begin end
      else if(pulse!=1||occupied||commit_seq==32'hffffffff)bresp<=2;
      else begin
       commit_req<=1;commit_seq<=commit_seq+1'b1;mt<=0;ms<=1;snapshot_valid<=0;
       commit_mode<=shadow_mode;commit_source<=shadow_source;commit_channel<=shadow_channel;
       commit_pattern<=shadow_pattern;commit_config_id<=shadow_config;
      end
     12'h050:if(pulse==0)begin end
      else if(pulse!=1||snapshot_seq==32'hffffffff)bresp<=2;
      else begin
       snapshot_seq<=snapshot_seq+1'b1;snapshot_valid<=1;snapshot_age<=0;snapshot_key<=key;
       snapshot_data<={origin_raw,latest_raw,test_count,adc_count,active_capture_id,32'd0,active_config_id,
        configured_config,configured_pattern,configured_channel,configured_source,configured_mode,flags};
      end
     default:bresp<=2;
    endcase
   end
   if(arvalid&&arready)begin
    rvalid<=1;rresp<=0;rdata<=0;
    if(araddr[1:0]!=0)rresp<=2;
    else if(araddr>=12'h080&&araddr<12'h0c8)rdata<=snapshot_data[(araddr-12'h080)*8+:32];
    else case(araddr)
     12'h000:rdata<=32'h31504143;
     12'h004:rdata<=32'h00010000;
     12'h008:rdata<={20'd0,clock_restart_pending,clock_fault_latched,mclk_ready_proc};
     12'h064:rdata<={clock_command_ack_seq,1'b0,clock_control_busy,clock_command_reason};
     12'h00c:rdata<=cs;12'h010:rdata<=command_seq;
     12'h014:rdata<={25'd0,running,stop_complete,quarantined,armed,clock_control_busy,service_busy,occupied};
     12'h020:rdata<=shadow_mode;12'h024:rdata<=shadow_source;12'h028:rdata<=shadow_channel;
     12'h02c:rdata<=shadow_pattern;12'h030:rdata<=shadow_config;
     12'h038:rdata<=ms;12'h03c:rdata<=commit_seq;
     12'h054:rdata<=2|((snapshot_valid&&snapshot_key==key)?32'h10:0);
     12'h058:rdata<=snapshot_seq;12'h05c:rdata<=snapshot_age[31:0];12'h060:rdata<=snapshot_age[63:32];
     default:rresp<=2;
    endcase
   end
  end
 end
 initial if(TIMEOUT_CYCLES<8)$error("capture register timeout must be >=8");
endmodule
