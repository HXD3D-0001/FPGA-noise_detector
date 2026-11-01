// CTR1: read-only producer contract snapshots. No capture/DMA commands.
// Same proc clock and global reset as CAP1/DAT1 and capture_processing_top.
module capture_contract_regs(
 input wire clk,aresetn,
 input wire[11:0]awaddr,input wire awvalid,output wire awready,
 input wire[31:0]wdata,input wire[3:0]wstrb,input wire wvalid,output wire wready,
 output reg[1:0]bresp,output reg bvalid,input wire bready,
 input wire[11:0]araddr,input wire arvalid,output wire arready,
 output reg[31:0]rdata,output reg[1:0]rresp,output reg rvalid,input wire rready,
 input wire armed,stop_complete,control_busy,capture_start,
 input wire[63:0]next_capture_id,active_capture_id,
 input wire[31:0]requested_config_id,active_config_id,
 input wire requested_test_mode,active_test_mode,channel_select,input wire[1:0]pattern_id,mode_active,
 input wire[63:0]frame_candidates,pcm_candidates,raw_candidates,energy_candidates,
 input wire[3:0]grid_meta_valid,
 input wire[255:0]grid_meta_capture,grid_meta_seq,grid_meta_raw,
 input wire[127:0]grid_meta_config
);
 reg ah,wh;reg[11:0]addr;reg[31:0]data;reg[3:0]strb;
 reg[31:0]snapshot_seq;reg snapshot_valid;reg[63:0]snapshot_age;
 reg[863:0]snapshot_data;
 reg started;reg[63:0]started_id;reg[31:0]started_config;
 reg started_test,started_channel;reg[1:0]started_pattern,started_mode;
 reg[255:0]started_seq;
 wire[255:0]candidates={energy_candidates,raw_candidates,pcm_candidates,frame_candidates};
 wire plan_ok=armed&&stop_complete&&!control_busy&&!capture_start&&next_capture_id!=0&&next_capture_id>active_capture_id&&requested_config_id!=0&&mode_active<3;
 wire active_ok=started&&!capture_start&&active_capture_id==started_id&&active_config_id==started_config&&active_test_mode==started_test&&mode_active==started_mode;
 reg[3:0]bound,errors;integer i;
 wire[3:0]required=started_mode==0?4'hb:4'h4;
 always@*begin
  bound=0;errors=0;
  for(i=0;i<4;i=i+1)if(required[i]&&grid_meta_valid[i])begin
   if(grid_meta_capture[i*64+:64]==started_id&&grid_meta_config[i*32+:32]==started_config&&grid_meta_seq[i*64+:64]==started_seq[i*64+:64])bound[i]=1;
   else if(grid_meta_capture[i*64+:64]>=started_id)errors[i]=1;
  end
 end
 reg[863:0]plan_image,active_image;integer j;
 always@*begin
  plan_image=0;active_image=0;
  plan_image[0+:32]=1;plan_image[32+:32]={30'd0,mode_active};
  plan_image[64+:32]={31'd0,requested_test_mode};plan_image[96+:32]={31'd0,channel_select};
  plan_image[128+:32]={30'd0,pattern_id};plan_image[160+:32]=requested_config_id;
  plan_image[192+:64]=next_capture_id;plan_image[256+:256]=candidates;
  plan_image[544+:32]=mode_active==0?32'hb:32'h4;
  active_image[0+:32]=2;active_image[32+:32]={30'd0,started_mode};
  active_image[64+:32]={31'd0,started_test};active_image[96+:32]={31'd0,started_channel};
  active_image[128+:32]={30'd0,started_pattern};active_image[160+:32]=started_config;
  active_image[192+:64]=started_id;active_image[256+:256]=started_seq;
  active_image[512+:32]={28'd0,bound};active_image[544+:32]={28'd0,required};
  for(j=0;j<4;j=j+1)if(bound[j])active_image[576+j*64+:64]=grid_meta_raw[j*64+:64];
  active_image[832+:32]={28'd0,errors};
 end
 wire stored_plan=snapshot_data[0+:32]==1;
 wire fresh=snapshot_valid&&!capture_start&&(stored_plan?(plan_ok&&snapshot_data==plan_image):(active_ok&&snapshot_data==active_image));
 wire[31:0]pulse=data & {{8{strb[3]}},{8{strb[2]}},{8{strb[1]}},{8{strb[0]}}};
 assign awready=aresetn&&!ah&&!bvalid;assign wready=aresetn&&!wh&&!bvalid;assign arready=aresetn&&!rvalid;
 always@(posedge clk or negedge aresetn)begin
  if(!aresetn)begin
   ah<=0;wh<=0;addr<=0;data<=0;strb<=0;bresp<=0;bvalid<=0;rdata<=0;rresp<=0;rvalid<=0;
   snapshot_seq<=0;snapshot_valid<=0;snapshot_age<=64'hffffffffffffffff;snapshot_data<=0;
   started<=0;started_id<=0;started_config<=0;started_test<=0;started_channel<=0;started_pattern<=0;started_mode<=0;started_seq<=0;
  end else begin
   if(snapshot_age!=64'hffffffffffffffff)snapshot_age<=snapshot_age+1'b1;
   // START invalidates even on a same-edge snapshot request; active metadata is
   // taken from the actually accepted reservation, never active_id+1.
   if(capture_start)begin
    started<=1;started_id<=next_capture_id;started_config<=requested_config_id;started_test<=requested_test_mode;
    started_channel<=channel_select;started_pattern<=pattern_id;started_mode<=mode_active;started_seq<=candidates;
    snapshot_valid<=0;
   end else if(snapshot_valid&&!fresh)snapshot_valid<=0;
   if(awvalid&&awready)begin ah<=1;addr<=awaddr;end
   if(wvalid&&wready)begin wh<=1;data<=wdata;strb<=wstrb;end
   if(bvalid&&bready)bvalid<=0;
   if(ah&&wh&&!bvalid)begin
    ah<=0;wh<=0;bvalid<=1;bresp<=2;
    if(addr==12'h008&&(pulse==1||pulse==2)&&snapshot_seq!=32'hffffffff&&!capture_start&&((pulse==1&&plan_ok)||(pulse==2&&active_ok)))begin
     snapshot_seq<=snapshot_seq+1'b1;snapshot_age<=0;snapshot_valid<=1;
     snapshot_data<=pulse==1?plan_image:active_image;bresp<=0;
    end
   end
   if(rvalid&&rready)rvalid<=0;
   if(arvalid&&arready)begin
    rvalid<=1;rresp<=0;rdata<=0;
    case(araddr)
     12'h000:rdata<=32'h31525443;
     12'h004:rdata<=32'h00010000;
     12'h00c:rdata<=snapshot_seq==0?32'd0:(fresh?32'd18:32'd2);
     12'h010:rdata<=snapshot_seq;
     12'h014:rdata<=snapshot_age[31:0];
     12'h018:rdata<=snapshot_age[63:32];
     default:if(araddr>=12'h080&&araddr<=12'h0e8&&araddr[1:0]==0)rdata<=snapshot_data[(araddr-12'h080)*8+:32];
             else rresp<=2;
    endcase
   end
  end
 end
endmodule
