module spectrum_shared_store #(parameter FLUSH_TIMEOUT_TICKS=5000000)(
 input wire clk,rst,reserve_valid, output reg grid_meta_valid,output reg[63:0]grid_meta_capture,grid_meta_seq,grid_meta_raw,
 output reg[31:0]grid_meta_config,
 output wire reserve_ready,
 input wire mode_valid,stopped,input wire[1:0]mode_select,output reg[1:0]mode_active,
 output wire mode_ready,banks_idle,output reg mode_rejected,
 input wire flush_req,dma_reset_done,input wire[31:0]dma_reset_seq,
 output reg flush_pending,flush_done,flush_timeout,output reg[31:0]flush_request_seq,
 input wire raw_valid,raw_analysis_reset,output wire raw_ready,
 input wire signed[23:0]raw_data,input wire[63:0]raw_capture_id,raw_index,
 input wire[31:0]raw_config_id,input wire[7:0]raw_flags,
 output reg[63:0]raw_candidate_count,raw_completed_count,raw_drop_count,raw_invalid_count,
 input wire [63:0] reserve_capture_id,reserve_first_raw,reserve_frame_seq,
 input wire [31:0] reserve_config_id,input wire [7:0] reserve_flags,
 input wire abort,commit,result_valid,output wire result_ready,
 input wire signed [23:0] result_real,result_imag,
 input wire [11:0] result_index,input wire [7:0] result_blk_exp,input wire result_last,
 output wire collect_done,output reg collect_error,output wire [7:0] collect_blk_exp,
 output wire packet_valid,input wire packet_ready,
 output wire [15:0] packet_type,packet_flags,
 output wire [63:0] packet_capture_id,packet_seq,packet_first_raw,
 output wire [31:0] packet_config_id,output wire [7:0] packet_blk_exp,
 input wire read_en,input wire [12:0] read_addr,
 output reg read_valid,output wire [31:0] read_data,input wire packet_done,
 output reg [63:0] completed_count,drop_count,invalid_count
);
 localparam [2:0] FREE=0,FILL=1,COMPLETE=2,READY=3,READING=4;
 // One physical pair of interleaved-word banks for both modes.
 (* ram_style="block" *) reg [31:0] ram0[0:8191],ram1[0:8191];
 reg [31:0] read0,read1,read0b,read1b;reg read_bank,read_lane;
 reg [2:0] state0,state1;
 reg [63:0] cap0,cap1,seq0,seq1,raw0,raw1;
 reg [31:0] cfg0,cfg1;reg [7:0] flags0,flags1,exp0,exp1;
 reg private_active,private_bank,reading,owner;
 reg [11:0] next_index;reg [7:0] active_exp;
 reg gap_pending,contract_valid,poison,sequence_valid;
 reg [63:0] highest_capture,last_sequence;reg [31:0] bound_config;
 wire newer=!contract_valid||reserve_capture_id>highest_capture;
 wire structural=reserve_capture_id!=0&&reserve_config_id!=0&&reserve_flags[0]&&
                 !reserve_flags[1]&&reserve_flags[7:6]==0&&
                 reserve_first_raw<=64'hffffffffffffffff-64'd196608;
 wire identity_ok=newer||(reserve_capture_id==highest_capture&&
                         reserve_config_id==bound_config&&!poison);
 wire sequence_ok=!sequence_valid||reserve_frame_seq>last_sequence;
 wire descriptor_ok=structural&&identity_ok&&sequence_ok;
 wire releasing=reading&&packet_done&&!flush_pending&&!flush_req;
 wire free0=state0==FREE||(releasing&&!owner);
 wire free1=state1==FREE||(releasing&&owner);
 assign reserve_ready=!rst&&!mode_valid&&mode_active==0&&!flush_pending&&!flush_req&&!abort&&!private_active&&(free0||free1||!descriptor_ok);
 wire reserved=reserve_valid&&reserve_ready;
 wire selected=(state0!=READY)||((state1==READY)&&(seq1<seq0));
 assign packet_valid=!rst&&!flush_pending&&!flush_req&&!reading&&(state0==READY||state1==READY);
 assign packet_type=mode_active==0?16'd1:16'd3;
 assign packet_flags={8'd0,selected?flags1:flags0};
 assign packet_capture_id=selected?cap1:cap0;
 assign packet_seq=selected?seq1:seq0;
 assign packet_first_raw=selected?raw1:raw0;
 assign packet_config_id=selected?cfg1:cfg0;
 assign packet_blk_exp=selected?exp1:exp0;
 wire [2:0] private_state=private_bank?state1:state0;
 assign collect_done=!rst&&mode_active==0&&!flush_pending&&!flush_req&&private_active&&private_state==COMPLETE;
 assign collect_blk_exp=active_exp;
 assign result_ready=!rst&&!mode_valid&&mode_active==0&&!flush_pending&&!flush_req&&!abort&&!commit&&private_active&&private_state==FILL;
 wire point_ok=result_index==next_index&&result_last==(next_index==12'd4095)&&
               result_blk_exp<=8'd12&&(next_index==0||result_blk_exp==active_exp);
 wire accepted_point=result_valid&&result_ready;
 wire write_enable=accepted_point&&point_ok;
 reg raw_fill_active,raw_fill_bank,raw_history,raw_gap;
 reg[12:0]raw_phase;reg[63:0]raw_previous;reg[31:0]flush_ticks;reg flush_exhausted;
 wire control_hold=flush_pending||flush_req;
 assign banks_idle=!rst&&!control_hold&&state0==FREE&&state1==FREE;
 assign mode_ready=banks_idle&&stopped;
 assign raw_ready=!rst&&!mode_valid&&mode_active!=0&&!control_hold&&!raw_analysis_reset;
 wire raw_accepted=raw_valid&&raw_ready;
 wire raw_newer=!contract_valid||raw_capture_id>highest_capture;
 wire raw_stale=contract_valid&&raw_capture_id<highest_capture;
 wire raw_structural=raw_capture_id!=0&&raw_config_id!=0&&raw_flags[0]&&raw_flags[7:6]==0;
 wire raw_identity=raw_newer||(raw_capture_id==highest_capture&&raw_config_id==bound_config&&!poison);
 wire raw_boundary=!raw_history||raw_newer||raw_flags[2]||raw_previous==64'hffffffffffffffff||raw_index!=raw_previous+1'b1;
 wire[12:0]raw_position=raw_boundary?13'd0:raw_phase;
 wire raw_usable=raw_accepted&&raw_structural&&!raw_flags[1]&&raw_identity&&
  (raw_position!=0||(raw_index<=64'hffffffffffffdfff&&raw_candidate_count!=64'hffffffffffffffff));
 wire raw_free0=free0||(raw_boundary&&raw_fill_active&&!raw_fill_bank);
 wire raw_free1=free1||(raw_boundary&&raw_fill_active&&raw_fill_bank);
 wire raw_have_free=raw_free0||raw_free1;
 wire raw_write_bank=raw_position==0?!raw_free0:raw_fill_bank;
 wire raw_write_enable=raw_usable&&((raw_position==0&&raw_have_free)||(raw_position!=0&&raw_fill_active));
 wire requested_read=!rst&&!control_hold&&reading&&!packet_done&&read_en;
 wire port_a_write=write_enable||raw_write_enable;
 wire port_a_bank=write_enable?private_bank:raw_write_bank;
 wire[12:0]port_a_address=write_enable?{next_index,1'b0}:raw_position;
 wire[31:0]port_a_data=write_enable?{{8{result_real[23]}},result_real}:{{8{raw_data[23]}},raw_data};
 wire request_lane=mode_active==0&&read_addr[0];
 wire[12:0]port_a0_address=(port_a_write&&!port_a_bank)?port_a_address:read_addr;
 wire[12:0]port_a1_address=(port_a_write&&port_a_bank)?port_a_address:read_addr;
 wire[12:0]port_b0_address=(write_enable&&!private_bank)?{next_index,1'b1}:read_addr;
 wire[12:0]port_b1_address=(write_enable&&private_bank)?{next_index,1'b1}:read_addr;
 assign read_data=read_bank?(read_lane?read1b:read1):(read_lane?read0b:read0);
 // Each bank has two write addresses for a complex FFT point, or one RAW word.
 // Read shares the even-word write port. FILL and READING never share a bank.
 // RAM arrays and read registers have no reset: ownership hides stale contents.
 always @(posedge clk)if((port_a_write&&!port_a_bank)||(requested_read&&!owner&&!request_lane))begin
  if(port_a_write&&!port_a_bank)ram0[port_a0_address]<=port_a_data;
  read0<=ram0[port_a0_address];
 end
 always @(posedge clk)if((port_a_write&&port_a_bank)||(requested_read&&owner&&!request_lane))begin
  if(port_a_write&&port_a_bank)ram1[port_a1_address]<=port_a_data;
  read1<=ram1[port_a1_address];
 end
 // True dual-port inference uses one clocked process and shared read/write
 // address per port. Both ports are read-first; ownership prevents collisions.
 always @(posedge clk)if((write_enable&&!private_bank)||(requested_read&&!owner&&request_lane))begin
  if(write_enable&&!private_bank)ram0[port_b0_address]<={{8{result_imag[23]}},result_imag};
  read0b<=ram0[port_b0_address];
 end
 always @(posedge clk)if((write_enable&&private_bank)||(requested_read&&owner&&request_lane))begin
  if(write_enable&&private_bank)ram1[port_b1_address]<={{8{result_imag[23]}},result_imag};
  read1b<=ram1[port_b1_address];
 end
 always @(posedge clk)begin
  if(requested_read)begin read_bank<=owner;read_lane<=request_lane;end
  read_valid<=requested_read;
 end
 always @(posedge clk)begin
  if(rst)begin
   state0<=FREE;state1<=FREE;cap0<=0;cap1<=0;seq0<=0;seq1<=0;raw0<=0;raw1<=0;
   cfg0<=0;cfg1<=0;flags0<=0;flags1<=0;exp0<=0;exp1<=0;
   private_active<=0;private_bank<=0;reading<=0;owner<=0;next_index<=0;active_exp<=0;
   gap_pending<=0;contract_valid<=0;poison<=0;sequence_valid<=0;
   highest_capture<=0;last_sequence<=0;bound_config<=0;
   collect_error<=0;mode_rejected<=0;flush_done<=0;completed_count<=0;drop_count<=0;invalid_count<=0;
   mode_active<=0;mode_rejected<=0;flush_pending<=0;flush_done<=0;flush_timeout<=0;flush_request_seq<=0;flush_ticks<=0;flush_exhausted<=0;
   raw_fill_active<=0;raw_fill_bank<=0;raw_history<=0;raw_gap<=0;raw_phase<=0;raw_previous<=0;
   raw_candidate_count<=0;raw_completed_count<=0;raw_drop_count<=0;raw_invalid_count<=0;
  end else begin
   collect_error<=0;mode_rejected<=0;flush_done<=0;
   if(releasing)begin
    if(owner)state1<=FREE;else state0<=FREE;
    reading<=0;
   end
   if(packet_valid&&packet_ready)begin
    owner<=selected;reading<=1;
    if(selected)state1<=READING;else state0<=READING;
   end
   if(!control_hold&&mode_active==0)begin
   if(abort)begin
    gap_pending<=1;
    if(private_active)begin
     if(private_bank)state1<=FREE;else state0<=FREE;
     private_active<=0;drop_count<=drop_count+1;
    end
   end else if(commit&&private_active)begin
    private_active<=0;
    if(private_state==COMPLETE)begin
     if(private_bank)begin state1<=READY;exp1<=active_exp;flags1<=flags1|(gap_pending?8'd4:8'd0);end
     else begin state0<=READY;exp0<=active_exp;flags0<=flags0|(gap_pending?8'd4:8'd0);end
     completed_count<=completed_count+1;gap_pending<=0;
    end else begin
     if(private_bank)state1<=FREE;else state0<=FREE;
     drop_count<=drop_count+1;invalid_count<=invalid_count+1;collect_error<=1;gap_pending<=1;
    end
   end else if(accepted_point)begin
    if(!point_ok)begin
     if(private_bank)state1<=FREE;else state0<=FREE;
     private_active<=0;drop_count<=drop_count+1;invalid_count<=invalid_count+1;
     collect_error<=1;gap_pending<=1;
    end else begin
     if(next_index==0)active_exp<=result_blk_exp;
     if(next_index==12'd4095)begin
      if(private_bank)state1<=COMPLETE;else state0<=COMPLETE;
     end else next_index<=next_index+1'b1;
    end
   end
   if(reserved)begin
    // Stale/malformed records never lower the capture or sequence barriers.
    if(newer&&structural)begin
     highest_capture<=reserve_capture_id;bound_config<=reserve_config_id;
     contract_valid<=1;poison<=0;
    end else if(contract_valid&&reserve_capture_id==highest_capture&&reserve_config_id!=bound_config)
     poison<=1;
    if(!descriptor_ok)begin
     invalid_count<=invalid_count+1;collect_error<=1;gap_pending<=1;
    end else begin
     last_sequence<=reserve_frame_seq;sequence_valid<=1;
     private_active<=1;private_bank<=!free0;next_index<=0;active_exp<=0;
     if(free0)begin
      state0<=FILL;cap0<=reserve_capture_id;seq0<=reserve_frame_seq;raw0<=reserve_first_raw;
      cfg0<=reserve_config_id;flags0<=reserve_flags|(gap_pending?8'd4:8'd0);
     end else begin
      state1<=FILL;cap1<=reserve_capture_id;seq1<=reserve_frame_seq;raw1<=reserve_first_raw;
      cfg1<=reserve_config_id;flags1<=reserve_flags|(gap_pending?8'd4:8'd0);
     end
    end
   end
   end
   if(!control_hold&&mode_active!=0)begin
    if(raw_analysis_reset||(raw_accepted&&!raw_usable&&!raw_stale))begin
     raw_gap<=1;raw_history<=0;raw_phase<=0;
     if(raw_fill_active)begin
      if(raw_fill_bank)state1<=FREE;else state0<=FREE;
      raw_drop_count<=raw_drop_count+1'b1;
     end
     raw_fill_active<=0;
    end
    if(raw_accepted)begin
     if(!raw_flags[1]&&!raw_usable)raw_invalid_count<=raw_invalid_count+1'b1;
     if(raw_newer&&raw_structural)begin contract_valid<=1;highest_capture<=raw_capture_id;bound_config<=raw_config_id;poison<=0;end
     else if(contract_valid&&raw_capture_id==highest_capture&&raw_config_id!=bound_config)poison<=1;
    end
    if(raw_usable)begin
     raw_history<=1;raw_previous<=raw_index;
     raw_phase<=raw_position+1'b1;
     if(raw_boundary&&raw_fill_active)begin
      if(raw_fill_bank)state1<=FREE;else state0<=FREE;
      raw_drop_count<=raw_drop_count+1'b1;raw_fill_active<=0;raw_gap<=1;
     end
     if(raw_position==0)begin
      raw_candidate_count<=raw_candidate_count+1'b1;
      raw_fill_bank<=!raw_free0;raw_fill_active<=raw_have_free;
      if(!raw_have_free)begin raw_drop_count<=raw_drop_count+1+(raw_boundary&&raw_fill_active);raw_gap<=1;end
      else if(raw_free0)begin
       state0<=FILL;cap0<=raw_capture_id;seq0<=raw_candidate_count;raw0<=raw_index;
       // A new identity begins its grid normally; producer GAP still survives.
       cfg0<=raw_config_id;flags0<=raw_flags|((!raw_newer&&(raw_boundary||raw_gap))?8'd4:0);exp0<=0;raw_gap<=0;
      end else begin
       state1<=FILL;cap1<=raw_capture_id;seq1<=raw_candidate_count;raw1<=raw_index;
       cfg1<=raw_config_id;flags1<=raw_flags|((!raw_newer&&(raw_boundary||raw_gap))?8'd4:0);exp1<=0;raw_gap<=0;
      end
     end else if(raw_fill_active)begin
      if(raw_fill_bank)flags1<=flags1|raw_flags;else flags0<=flags0|raw_flags;
      if(raw_position==8191)begin
       if(raw_fill_bank)state1<=READY;else state0<=READY;
       raw_fill_active<=0;raw_completed_count<=raw_completed_count+1'b1;
      end
     end
    end
   end
   if(mode_valid)begin
    if(mode_ready&&mode_select<3)begin
     mode_active<=mode_select;raw_history<=0;raw_phase<=0;raw_gap<=1;
    end else mode_rejected<=1;
   end
   if(flush_req&&!flush_pending)begin
    // Do not release any published/reading bank until DMA reset is confirmed.
    flush_pending<=1;flush_ticks<=0;flush_timeout<=0;flush_exhausted<=0;
    if(flush_request_seq==32'hffffffff)begin flush_timeout<=1;flush_exhausted<=1;end
    else flush_request_seq<=flush_request_seq+1'b1;
   end
   if(flush_pending)begin
    if(dma_reset_done&&dma_reset_seq==flush_request_seq&&!flush_exhausted)begin
     state0<=FREE;state1<=FREE;private_active<=0;reading<=0;raw_fill_active<=0;
     raw_history<=0;raw_phase<=0;raw_gap<=1;
     gap_pending<=gap_pending||(state0!=FREE)||(state1!=FREE);
     if(mode_active==0)drop_count<=drop_count+(state0!=FREE)+(state1!=FREE);
     else raw_drop_count<=raw_drop_count+(state0!=FREE)+(state1!=FREE);
     flush_pending<=0;flush_done<=1;
    end else if(flush_ticks>=FLUSH_TIMEOUT_TICKS-1)flush_timeout<=1;
    else flush_ticks<=flush_ticks+1'b1;
   end
  end
 end
 // First candidate metadata is captured at the producer's real grid binding,
 // before bank availability/publication. Only global reset or a newer capture
 // replaces it; local history/DMA recovery never relabels the first candidate.
 always@(posedge clk)begin
  if(rst)begin grid_meta_valid<=0;grid_meta_capture<=0;grid_meta_config<=0;grid_meta_seq<=0;grid_meta_raw<=0;end
  else if((raw_usable)&&(!grid_meta_valid||raw_capture_id>grid_meta_capture))begin
   grid_meta_valid<=1;grid_meta_capture<=raw_capture_id;grid_meta_config<=raw_config_id;
   grid_meta_seq<=raw_candidate_count;grid_meta_raw<=raw_index;
  end
 end
endmodule

