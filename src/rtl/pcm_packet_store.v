// Two complete, immutable PCM banks; acquisition never waits for packet/DMA.
// RAW uses SAMPLE_COUNT=8192, RAW_STRIDE=1, PACKET_TYPE=3 in a shared-bank mode.
// analysis_reset discards only incomplete history, preserving published banks.
module pcm_packet_store #(
 parameter integer SAMPLE_COUNT=256,RAW_STRIDE=48,PACKET_TYPE=2,ENABLE_BANK_FLUSH=0,FIXED_CAPTURE_GRID=0
)(input wire clk,rst,analysis_reset,s_valid,output wire s_ready,
 input wire bank_flush,output reg[63:0]flush_count,
 input wire signed [23:0] s_data,input wire [63:0] s_capture_id,s_raw_index,
 input wire [31:0] s_config_id,input wire [7:0] s_flags,
 output reg grid_meta_valid,output reg[63:0]grid_meta_capture,grid_meta_seq,grid_meta_raw,
 output reg[31:0]grid_meta_config,
 output wire packet_valid,input wire packet_ready,
 output wire [15:0] packet_type,packet_flags,
 output wire [63:0] packet_capture_id,packet_seq,packet_first_raw,
 output wire [31:0] packet_config_id,output wire [7:0] packet_blk_exp,
 input wire read_en,input wire [12:0] read_addr,
 output reg read_valid,output wire [31:0] read_data,input wire packet_done,
 output reg [63:0] candidate_count,drop_count,invalid_count,
 output wire work_pending
);
 // Preserve source metadata at the original handshake while a discontinuity
 // is calculated. Normal sequential samples retain single-cycle throughput.
 wire sample_valid;wire signed[23:0]sample_data;wire[63:0]sample_capture_id,sample_raw_index;
 wire[31:0]sample_config_id;wire[7:0]sample_flags;
 wire[63:0]grid_sample_index,calculated_seq,calculated_missed;wire grid_remainder_nonzero,calculated_bad;
 wire early_boundary;
 localparam FREE=0,FILL=1,READY=2,READING=3;
 wire flushing=ENABLE_BANK_FLUSH&&bank_flush;
 (* ram_style="block" *) reg [31:0] ram0[0:SAMPLE_COUNT-1],ram1[0:SAMPLE_COUNT-1];
 reg [31:0] read0,read1;reg read_bank;
 assign read_data=read_bank?read1:read0;
 reg [1:0] state0,state1;
 reg [63:0] cap0,cap1,seq0,seq1,first0,first1;
 reg [31:0] cfg0,cfg1;reg [7:0] flags0,flags1;
 reg owner,reading,fill_bank,fill_active,history,contract_valid,poison,gap_pending;
 reg [63:0] contract_capture,last_raw;reg [31:0] contract_config;
 reg [12:0] phase;
 reg grid_valid;reg[63:0]grid_capture,grid_first_raw,grid_first_seq;
 wire selected=(state0!=READY)||((state1==READY)&&(seq1<seq0));
 assign packet_valid=!rst&&!flushing&&!reading&&(state0==READY||state1==READY);
 assign packet_type=PACKET_TYPE;assign packet_blk_exp=0;
 assign packet_flags={8'b0,selected?flags1:flags0};
 assign packet_capture_id=selected?cap1:cap0;
 assign packet_seq=selected?seq1:seq0;
 assign packet_first_raw=selected?first1:first0;
 assign packet_config_id=selected?cfg1:cfg0;
 
 wire accepted=sample_valid;
 assign work_pending=!rst&&!analysis_reset&&!flushing&&!s_ready;
 wire newer=!contract_valid||sample_capture_id>contract_capture;
 wire stale=contract_valid&&sample_capture_id<contract_capture;
 wire structural=sample_capture_id!=0&&sample_config_id!=0&&sample_flags[0]&&sample_flags[7:6]==0;
 wire identity_ok=newer||(sample_capture_id==contract_capture&&sample_config_id==contract_config&&!poison);
 // Bind the usable-sample grid separately from warmup metadata. Local buffer
 // recovery cannot rebase a capture's first anchor or lifetime sequence.
 wire new_grid=!grid_valid||sample_capture_id>grid_capture;
 wire[63:0]grid_delta=sample_raw_index-grid_first_raw;
 wire[63:0]grid_index=grid_sample_index>>$clog2(SAMPLE_COUNT);
 wire[63:0]grid_seq=calculated_seq;
 wire grid_bad=FIXED_CAPTURE_GRID&&(calculated_bad||(!new_grid&&(sample_raw_index<grid_first_raw||grid_remainder_nonzero)));
 wire usable=accepted&&sample_capture_id!=0&&sample_config_id!=0&&sample_flags[0]&&!sample_flags[1]&&
             sample_flags[7:6]==0&&identity_ok&&!grid_bad;
 wire boundary=!history||new_grid||sample_flags[2]||last_raw>64'hffffffffffffffff-RAW_STRIDE||
               sample_raw_index!=last_raw+RAW_STRIDE;
 wire [12:0] position=FIXED_CAPTURE_GRID?(new_grid?13'd0:grid_sample_index[$clog2(SAMPLE_COUNT)-1:0]):(boundary?13'd0:phase);
 wire[63:0]grid_missed=FIXED_CAPTURE_GRID&&usable?calculated_missed:64'd0;
 wire free0=state0==FREE||(reading&&!owner&&packet_done)||(boundary&&fill_active&&!fill_bank);
 wire free1=state1==FREE||(reading&&owner&&packet_done)||(boundary&&fill_active&&fill_bank);
 wire have_free=free0||free1;
 wire write_bank=position==0?!free0:fill_bank;
 wire write_enable=usable&&((position==0&&have_free)||(position!=0&&fill_active&&!boundary));
 // One counter adder covers the original branch priorities. A boundary can
 // discard the old partial packet and fail to reserve its new candidate.
 wire[1:0]local_drop=flushing?{1'b0,fill_active}:usable?
  ({1'b0,(boundary&&fill_active)}+{1'b0,(position==0&&!have_free)}):
  {1'b0,((analysis_reset||early_boundary||(accepted&&!usable&&!stale))&&fill_active)};
 wire[63:0]drop_increment=(flushing?64'd0:grid_missed)+local_drop;
 always@(posedge clk)begin
  if(rst)drop_count<=0;
  else if(drop_increment!=0)drop_count<=drop_count+drop_increment;
 end
 generate if(FIXED_CAPTURE_GRID)begin:g_grid_input
  reg signed[23:0]saved_data;reg[63:0]saved_capture,saved_raw;
  reg[31:0]saved_config;reg[7:0]saved_flags;
  wire calculation_ready,calculation_valid,held;
  // Published PCM banks are immutable across gaps; discard only old partial
  // ownership at the original slow handshake, before any later write/result.
  assign early_boundary=s_valid&&s_ready&&!calculation_valid&&!stale&&
   (!structural||!identity_ok||sample_flags[1]||boundary||
    candidate_count==64'hffffffffffffffff||(!new_grid&&sample_raw_index<grid_first_raw));
  wire source_new=!grid_valid||s_capture_id>grid_capture;
  wire source_sequential=history&&s_capture_id==grid_capture&&last_raw<=64'hffffffffffffffff-RAW_STRIDE&&s_raw_index==last_raw+RAW_STRIDE;
  wire cancelled=analysis_reset||flushing;
  capture_grid_divider #(.DIVISOR(RAW_STRIDE),.HOP_SHIFT($clog2(SAMPLE_COUNT))) grid_math(
   .clk(clk),.rst(rst),.cancel(cancelled),.start(s_valid),.new_anchor(source_new),.sequential(source_sequential),
   .commit(usable),.delta(source_new?64'd0:s_raw_index-grid_first_raw),
   .first_seq(grid_first_seq),.candidate_count(candidate_count),
   .ready(calculation_ready),.valid(calculation_valid),.held(held),
   .quotient(grid_sample_index),.remainder_nonzero(grid_remainder_nonzero),
   .grid_seq(calculated_seq),.grid_missed(calculated_missed),.grid_bad(calculated_bad));
  assign s_ready=calculation_ready;
  assign sample_valid=calculation_valid;
  assign sample_data=held?saved_data:s_data;
  assign sample_capture_id=held?saved_capture:s_capture_id;
  assign sample_raw_index=held?saved_raw:s_raw_index;
  assign sample_config_id=held?saved_config:s_config_id;
  assign sample_flags=held?saved_flags:s_flags;
  always@(posedge clk)if(s_valid&&s_ready)begin
   saved_data<=s_data;saved_capture<=s_capture_id;saved_raw<=s_raw_index;
   saved_config<=s_config_id;saved_flags<=s_flags;
  end
 end else begin:g_segment_input
  assign early_boundary=0;
  assign s_ready=!rst&&!(analysis_reset||flushing);assign sample_valid=s_valid&&s_ready;
  assign sample_data=s_data;assign sample_capture_id=s_capture_id;assign sample_raw_index=s_raw_index;
  assign sample_config_id=s_config_id;assign sample_flags=s_flags;
  assign grid_sample_index=0;assign grid_remainder_nonzero=0;
  assign calculated_seq=0;assign calculated_missed=0;assign calculated_bad=0;
 end endgenerate
 // Pure synchronous memory ports, with no array reset, infer block RAM.
 always @(posedge clk)begin
  if(write_enable)begin
   if(write_bank)ram1[position]<={{8{sample_data[23]}},sample_data};
   else ram0[position]<={{8{sample_data[23]}},sample_data};
  end
  read_valid<=!rst&&!flushing&&reading&&read_en&&read_addr<SAMPLE_COUNT;
  if(reading&&read_en&&read_addr<SAMPLE_COUNT)begin
   read0<=ram0[read_addr];read1<=ram1[read_addr];read_bank<=owner;
  end
 end
 always @(posedge clk)begin
  if(rst)begin
   state0<=FREE;state1<=FREE;cap0<=0;cap1<=0;seq0<=0;seq1<=0;first0<=0;first1<=0;
   cfg0<=0;cfg1<=0;flags0<=0;flags1<=0;owner<=0;reading<=0;fill_bank<=0;
   fill_active<=0;history<=0;contract_valid<=0;poison<=0;gap_pending<=0;contract_capture<=0;
   contract_config<=0;last_raw<=0;phase<=0;candidate_count<=0;invalid_count<=0;flush_count<=0;grid_valid<=0;grid_capture<=0;grid_first_raw<=0;grid_first_seq<=0;
  end else if(flushing)begin
   flush_count<=flush_count+(state0==READY||state0==READING)+(state1==READY||state1==READING);
   state0<=FREE;state1<=FREE;reading<=0;fill_active<=0;gap_pending<=1;
   // Single-flow recovery retains the original capture candidate grid. A
   // global analysis reset accompanies a later new capture and can clear it.
   if(analysis_reset&&!FIXED_CAPTURE_GRID)begin phase<=0;history<=0;end
  end else begin
   if(reading&&packet_done)begin
    if(owner)state1<=FREE;else state0<=FREE;reading<=0;
   end
   if(packet_valid&&packet_ready)begin
    owner<=selected;reading<=1;if(selected)state1<=READING;else state0<=READING;
   end
   if(analysis_reset||early_boundary||(accepted&&!usable&&!stale))begin
    gap_pending<=1;
    if(fill_active)begin
     if(fill_bank)state1<=FREE;else state0<=FREE;
    end
    fill_active<=0;if(!FIXED_CAPTURE_GRID)begin phase<=0;history<=0;end
   end
   if(accepted)begin
    if(!sample_flags[1]&&!usable)invalid_count<=invalid_count+1;
    if(newer&&structural)begin contract_valid<=1;contract_capture<=sample_capture_id;contract_config<=sample_config_id;poison<=0;end
    else if(contract_valid&&sample_capture_id==contract_capture&&sample_config_id!=contract_config)poison<=1;
    if(grid_bad&&!stale&&!new_grid)poison<=1;
   end
   if(usable)begin
    if(new_grid)begin grid_valid<=1;grid_capture<=sample_capture_id;grid_first_raw<=sample_raw_index;grid_first_seq<=candidate_count;end
    if(FIXED_CAPTURE_GRID&&grid_seq>=candidate_count)candidate_count<=grid_seq+1'b1;
    if(grid_missed!=0)begin gap_pending<=1;end
    history<=1;last_raw<=sample_raw_index;
    phase<=(position==SAMPLE_COUNT-1)?13'd0:position+1'b1;
    if(boundary&&fill_active)begin
     if(fill_bank)state1<=FREE;else state0<=FREE;
     fill_active<=0;gap_pending<=1;
    end
    if(position==0)begin
     if(!FIXED_CAPTURE_GRID)candidate_count<=candidate_count+1;
     fill_bank<=!free0;fill_active<=have_free;
     if(!have_free)begin gap_pending<=1;end
     else if(free0)begin
      state0<=FILL;cap0<=sample_capture_id;seq0<=FIXED_CAPTURE_GRID?grid_seq:candidate_count;first0<=sample_raw_index;
      cfg0<=sample_config_id;flags0<=sample_flags|((!new_grid&&(boundary||gap_pending||grid_missed!=0))?8'd4:8'd0);gap_pending<=0;
     end else begin
      state1<=FILL;cap1<=sample_capture_id;seq1<=FIXED_CAPTURE_GRID?grid_seq:candidate_count;first1<=sample_raw_index;
      cfg1<=sample_config_id;flags1<=sample_flags|((!new_grid&&(boundary||gap_pending||grid_missed!=0))?8'd4:8'd0);gap_pending<=0;
     end
    end else if(fill_active)begin
     if(fill_bank)flags1<=flags1|sample_flags;else flags0<=flags0|sample_flags;
     if(position==SAMPLE_COUNT-1)begin
      if(fill_bank)state1<=READY;else state0<=READY;fill_active<=0;
     end
    end
   end
  end
 end
 initial if(!((SAMPLE_COUNT==256&&RAW_STRIDE==48&&PACKET_TYPE==2)||
              (SAMPLE_COUNT==8192&&RAW_STRIDE==1&&PACKET_TYPE==3)))
  $error("unsupported PCM/RAW packet geometry");
 // First candidate metadata is captured at the producer's real grid binding,
 // before bank availability/publication. Only global reset or a newer capture
 // replaces it; local history/DMA recovery never relabels the first candidate.
 always@(posedge clk)begin
  if(rst)begin grid_meta_valid<=0;grid_meta_capture<=0;grid_meta_config<=0;grid_meta_seq<=0;grid_meta_raw<=0;end
  else if((FIXED_CAPTURE_GRID&&usable&&new_grid)&&(!grid_meta_valid||sample_capture_id>grid_meta_capture))begin
   grid_meta_valid<=1;grid_meta_capture<=sample_capture_id;grid_meta_config<=sample_config_id;
   grid_meta_seq<=candidate_count;grid_meta_raw<=sample_raw_index;
  end
 end
endmodule
