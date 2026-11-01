// Two complete-frame input banks; the reader never backpressures physical input.
// rst is boot/global. frame_reset and input discontinuities invalidate ownership
// and explicitly pulse frame_abort when cancelling an offered/partly read frame.
// The future FFT consumer MUST reset its partial-input state on frame_abort.
// Candidate sequence/counters survive all local boundaries. HANN_FILE must be
// packaged as a Vivado memory source; the default is relative to project root.
module frame_window #(
 parameter HANN_FILE="generated/hann4096.mem", parameter integer RAW_STRIDE=48,
 parameter integer FIXED_CAPTURE_GRID=0
)(input wire clk,rst,frame_reset,
 input wire s_valid,output wire s_ready,input wire signed[23:0]s_data,
 input wire[63:0]s_capture_id,s_raw_index,input wire[31:0]s_config_id,input wire[7:0]s_flags,
 output reg grid_meta_valid,output reg[63:0]grid_meta_capture,grid_meta_seq,grid_meta_raw,
 output reg[31:0]grid_meta_config,
 output reg m_valid,input wire m_ready,output reg signed[23:0]m_data,
 output reg[63:0]m_capture_id,m_first_raw,m_frame_seq,output reg[31:0]m_config_id,
 output reg[7:0]m_flags,output reg[11:0]m_index,output reg m_last,frame_abort,
 output reg[63:0]candidate_count,drop_count,saturation_count,rejected_count,
 output wire capture_poisoned,output reg[3:0]input_fault,
 output wire work_pending);
 // Preserve source metadata at the original handshake while a discontinuity
 // is calculated. Normal sequential samples retain single-cycle throughput.
 wire sample_valid;wire signed[23:0]sample_data;wire[63:0]sample_capture_id,sample_raw_index;
 wire[31:0]sample_config_id;wire[7:0]sample_flags;
 wire[63:0]grid_sample_index,calculated_seq,calculated_missed;wire grid_remainder_nonzero,calculated_bad;
 wire early_boundary;
 localparam[1:0]FREE=0,FILLING=1,READY=2,READING=3;
 (* ram_style="block" *)reg signed[23:0]bank0[0:4095],bank1[0:4095];
 (* rom_style="block" *)reg signed[17:0]hann[0:4095];
 reg[1:0]state0,state1;reg[11:0]write0,write1;
 reg[63:0]capture0,capture1,first0,first1,seq0,seq1;
 reg[31:0]config0,config1;reg[7:0]flags0,flags1;
 reg have_input;reg[63:0]previous_capture,previous_raw;reg[31:0]previous_config;
 reg contract_valid,poisoned,history_gap;reg[63:0]highest_capture;reg[31:0]bound_config;
 reg[10:0]hop_position;
 // The fixed-grid profile separates capture identity from disposable frame
 // history. A local abort cannot move the first-candidate anchor or sequence.
 reg grid_valid;reg[63:0]grid_first_raw,grid_first_seq;
 reg reader_active,reader_bank,read_valid;reg[12:0]read_position;
 reg signed[23:0]memory_read0,memory_read1;reg signed[17:0]read_coefficient;
 reg read_stage_bank;
 wire signed[23:0]read_data=read_stage_bank?memory_read1:memory_read0;
 reg[11:0]read_index;reg[63:0]read_capture,read_first,read_seq;
 reg[31:0]read_config;reg[7:0]read_flags;
 wire accept_input=sample_valid;
 wire metadata_invalid=sample_capture_id==0||sample_config_id==0;
 wire flags_invalid=!sample_flags[0]||(|sample_flags[7:6]);
 wire stale=!metadata_invalid&&contract_valid&&sample_capture_id<highest_capture;
 wire config_error=contract_valid&&sample_capture_id==highest_capture&&sample_config_id!=bound_config;
 wire blocked=poisoned&&sample_capture_id<=highest_capture;
 wire grid_new_capture=!metadata_invalid&&!flags_invalid&&(!contract_valid||sample_capture_id>highest_capture);
 localparam integer GRID_HOP=2048*RAW_STRIDE;
 wire[63:0]grid_delta=sample_raw_index-grid_first_raw;
 wire[63:0]grid_index=grid_sample_index>>11;
 wire[63:0]grid_seq=calculated_seq;
 wire grid_due=!grid_valid||grid_new_capture||(grid_sample_index[10:0]==0);
 wire grid_bad=FIXED_CAPTURE_GRID&&(calculated_bad||(grid_valid&&!grid_new_capture&&(sample_raw_index<grid_first_raw||grid_remainder_nonzero)));
 wire reject_input=metadata_invalid||flags_invalid||stale||config_error||blocked||grid_bad;
 wire new_capture_good=!metadata_invalid&&!flags_invalid&&(!contract_valid||sample_capture_id>highest_capture);
 wire unusable=reject_input||sample_flags[1];
 wire raw_mismatch=have_input&&sample_capture_id==previous_capture&&
  (previous_raw>64'hffffffffffffffff-RAW_STRIDE||sample_raw_index!=previous_raw+RAW_STRIDE);
 wire discontinuity=sample_flags[2]||new_capture_good||raw_mismatch;
 wire clear_now=frame_reset||early_boundary||(accept_input&&!stale&&(unusable||discontinuity));
 wire[7:0]effective_flags=sample_flags|((raw_mismatch||(history_gap&&!new_capture_good))?8'h04:8'h00);
 wire advance_output=!m_valid||m_ready;
 wire release_reader=m_valid&&m_ready&&m_last;
 wire release0=release_reader&&!reader_bank;
 wire release1=release_reader&&reader_bank;
 wire grid_reserve=grid_due&&grid_seq>=candidate_count;
 wire[63:0]grid_missed=FIXED_CAPTURE_GRID&&accept_input&&!unusable?calculated_missed:64'd0;
 wire new_segment=clear_now&&!frame_reset&&accept_input&&!unusable&&(!FIXED_CAPTURE_GRID||grid_reserve);
 wire reserve_candidate=!clear_now&&accept_input&&!unusable&&
  (FIXED_CAPTURE_GRID?grid_reserve:(!have_input||hop_position==0));
 wire choose0=state0==FREE||release0;
 wire choose1=!choose0&&(state1==FREE||release1);
 wire write_enable0=!rst&&(new_segment||(!clear_now&&accept_input&&!unusable&&(state0==FILLING||(reserve_candidate&&choose0))));
 wire write_enable1=!rst&&!clear_now&&accept_input&&!unusable&&(state1==FILLING||(reserve_candidate&&choose1));
 wire[11:0]write_address0=(new_segment||(reserve_candidate&&choose0))?12'd0:write0;
 wire[11:0]write_address1=(reserve_candidate&&choose1)?12'd0:write1;
 wire read_issue=!rst&&!clear_now&&advance_output&&reader_active&&read_position<4096;
 
 assign capture_poisoned=poisoned;
 // Preserve final-handshake release priority while sharing one u64 adder.
 wire[1:0]local_drop=clear_now?
  ({1'b0,((state0!=FREE)&&!release0)}+{1'b0,((state1!=FREE)&&!release1)}):
  {1'b0,(reserve_candidate&&!choose0&&!choose1)};
 wire[63:0]drop_increment=grid_missed+local_drop;
 always@(posedge clk or posedge rst)begin
  if(rst)drop_count<=0;
  else if(drop_increment!=0)drop_count<=drop_count+drop_increment;
 end
 // A completed bank and both reader stages retain ownership even before the
 // first valid output. Incomplete FILL history may be discarded after STOP.
 assign work_pending=!rst&&!frame_reset&&(!s_ready||state0==READY||state1==READY||
  state0==READING||state1==READING||reader_active||read_valid||m_valid);

 generate if(FIXED_CAPTURE_GRID)begin:g_grid_input
  reg signed[23:0]saved_data;reg[63:0]saved_capture,saved_raw;
  reg[31:0]saved_config;reg[7:0]saved_flags;
  wire calculation_ready,calculation_valid,held;
  // A slow source handshake must isolate old ownership on that same edge.
  // The saved sample still completes its arithmetic; this does not cancel it.
  assign early_boundary=s_valid&&s_ready&&!calculation_valid&&!stale&&
   (metadata_invalid||flags_invalid||config_error||blocked||sample_flags[1]||
    discontinuity||candidate_count==64'hffffffffffffffff||
    (grid_valid&&sample_raw_index<grid_first_raw));
  wire source_new=!grid_valid||s_capture_id>highest_capture;
  wire source_sequential=have_input&&s_capture_id==previous_capture&&previous_raw<=64'hffffffffffffffff-RAW_STRIDE&&s_raw_index==previous_raw+RAW_STRIDE;
  wire cancelled=frame_reset;
  capture_grid_divider #(.DIVISOR(RAW_STRIDE),.HOP_SHIFT(11)) grid_math(
   .clk(clk),.rst(rst),.cancel(cancelled),.start(s_valid),.new_anchor(source_new),.sequential(source_sequential),
   .commit(!unusable),.delta(source_new?64'd0:s_raw_index-grid_first_raw),
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
  assign s_ready=!rst&&!(frame_reset);assign sample_valid=s_valid&&s_ready;
  assign sample_data=s_data;assign sample_capture_id=s_capture_id;assign sample_raw_index=s_raw_index;
  assign sample_config_id=s_config_id;assign sample_flags=s_flags;
  assign grid_sample_index=0;assign grid_remainder_nonzero=0;
  assign calculated_seq=0;assign calculated_missed=0;assign calculated_bad=0;
 end endgenerate
 // Single synchronous write port and synchronous read port per bank. No RAM
 // reset or async-reset write process: ownership excludes stale words instead.
 always@(posedge clk)begin
  if(write_enable0)bank0[write_address0]<=sample_data;
  if(write_enable1)bank1[write_address1]<=sample_data;
  if(read_issue)begin
   memory_read0<=bank0[read_position[11:0]];
   memory_read1<=bank1[read_position[11:0]];
  end
 end
 always@(posedge clk)if(read_issue)read_coefficient<=hann[read_position[11:0]];

 function automatic[24:0]window_product;
  input signed[23:0]sample;input signed[17:0]coefficient;
  reg signed[41:0]product;reg signed[42:0]extended,rounded;
  begin
   product=sample*coefficient;extended={product[41],product};
   if(extended<0)rounded=-((-extended+43'sd65536)>>>17);
   else rounded=(extended+43'sd65536)>>>17;
   if(rounded>43'sd8388607)window_product={1'b1,24'h7fffff};
   else if(rounded< -43'sd8388608)window_product={1'b1,24'h800000};
   else window_product={1'b0,rounded[23:0]};
  end
 endfunction
 wire[24:0]rounded_product=window_product(read_data,read_coefficient);
 integer coefficient_index;
 initial begin
  $readmemh(HANN_FILE,hann);
`ifndef SYNTHESIS
  if(RAW_STRIDE<=0)$error("RAW_STRIDE must be positive");
  // Proper Hann coefficients cannot amplify a signed24 input, so arithmetic
  // SATURATION is impossible; inherited frame flags are complete before READY.
  for(coefficient_index=0;coefficient_index<4096;coefficient_index=coefficient_index+1)
   if((^hann[coefficient_index])===1'bx||hann[coefficient_index][17])$error("invalid Q1.17 nonnegative Hann ROM");
`endif
 end
 always@(posedge clk or posedge rst)begin
  if(rst)begin
   state0<=FREE;state1<=FREE;write0<=0;write1<=0;capture0<=0;capture1<=0;first0<=0;first1<=0;seq0<=0;seq1<=0;
   config0<=0;config1<=0;flags0<=0;flags1<=0;have_input<=0;previous_capture<=0;previous_raw<=0;previous_config<=0;hop_position<=0;
   contract_valid<=0;poisoned<=0;history_gap<=0;highest_capture<=0;bound_config<=0;rejected_count<=0;input_fault<=0;
   grid_valid<=0;grid_first_raw<=0;grid_first_seq<=0;
   reader_active<=0;reader_bank<=0;read_position<=0;read_valid<=0;read_stage_bank<=0;read_index<=0;
   read_capture<=0;read_first<=0;read_seq<=0;read_config<=0;read_flags<=0;
   m_valid<=0;m_data<=0;m_capture_id<=0;m_first_raw<=0;m_frame_seq<=0;m_config_id<=0;m_flags<=0;m_index<=0;m_last<=0;frame_abort<=0;
   candidate_count<=0;saturation_count<=0;
  end else begin
   frame_abort<=0;
   // Capture/config binding and its highest-identity barrier outlive local
   // history resets. Late/stale identities never lower or unlock this barrier.
   if(accept_input)begin
    if(reject_input)begin
     rejected_count<=rejected_count+1'b1;
     if(stale)input_fault<=5;
     else if(sample_capture_id==0)input_fault<=1;
     else if(sample_config_id==0)input_fault<=2;
     else if(flags_invalid)input_fault<=3;
     else if(config_error)input_fault<=4;
     else if(grid_bad)input_fault<=7;
     else input_fault<=6;
    end
    if(config_error&&!stale)poisoned<=1;
    if(grid_bad&&!stale)poisoned<=1;
    if(new_capture_good)begin
     contract_valid<=1;highest_capture<=sample_capture_id;bound_config<=sample_config_id;poisoned<=0;history_gap<=0;input_fault<=0;
     grid_valid<=0;
    end else if(reject_input&&!stale)history_gap<=1;
   end
   if(FIXED_CAPTURE_GRID&&accept_input&&!unusable)begin
    if(!grid_valid||new_capture_good)begin
     grid_valid<=1;grid_first_raw<=sample_raw_index;grid_first_seq<=candidate_count;
    end
    // Count every skipped physical candidate, including the current candidate
    // when its first sample was lost. Never relabel it as a later frame.
    if(grid_seq>=candidate_count)candidate_count<=grid_seq+1'b1;
   end
   if(clear_now)begin
    // A final handshake on this same edge has already completed its frame.
    // Abort/drop only the remaining incomplete ownership, never that frame.
    frame_abort<=(reader_active||read_valid||m_valid)&&!release_reader;
    state0<=FREE;state1<=FREE;reader_active<=0;read_valid<=0;m_valid<=0;read_position<=0;
    if(!FIXED_CAPTURE_GRID)begin have_input<=0;hop_position<=0;end
    // A usable discontinuity sample is the first sample of the NEW history.
    if(frame_reset||(FIXED_CAPTURE_GRID&&accept_input&&!unusable&&discontinuity))history_gap<=1;
    if(!frame_reset&&accept_input&&!unusable)begin
     have_input<=1;previous_capture<=sample_capture_id;previous_raw<=sample_raw_index;previous_config<=sample_config_id;
    end
    if(new_segment)begin
     state0<=FILLING;write0<=1;capture0<=sample_capture_id;first0<=sample_raw_index;
     seq0<=FIXED_CAPTURE_GRID?grid_seq:candidate_count;config0<=sample_config_id;flags0<=effective_flags;
     if(!FIXED_CAPTURE_GRID)candidate_count<=candidate_count+1'b1;
     history_gap<=0;
     have_input<=1;previous_capture<=sample_capture_id;previous_raw<=sample_raw_index;previous_config<=sample_config_id;hop_position<=1;
    end
   end else begin
    // Synchronous bank/ROM read followed by multiply/round/output register.
    // Both elastic stages freeze together whenever the offered output stalls.
    if(advance_output)begin
     m_valid<=read_valid;
     if(read_valid)begin
      m_data<=rounded_product[23:0];m_capture_id<=read_capture;m_first_raw<=read_first;m_frame_seq<=read_seq;
      m_config_id<=read_config;m_flags<=read_flags|(rounded_product[24]?8'h08:8'h00);m_index<=read_index;m_last<=read_index==4095;
      if(rounded_product[24])saturation_count<=saturation_count+1'b1;
     end
     read_valid<=0;
     if(reader_active&&read_position<4096)begin
      read_stage_bank<=reader_bank;read_index<=read_position[11:0];
      read_capture<=reader_bank?capture1:capture0;read_first<=reader_bank?first1:first0;read_seq<=reader_bank?seq1:seq0;
      read_config<=reader_bank?config1:config0;read_flags<=reader_bank?flags1:flags0;
      read_valid<=1;read_position<=read_position+1'b1;
     end
    end
    if(release_reader)begin
     reader_active<=0;if(reader_bank)state1<=FREE;else state0<=FREE;
    end
    if(!reader_active)begin
     if(state0==READY&&(state1!=READY||seq0<seq1))begin reader_active<=1;reader_bank<=0;read_position<=0;state0<=READING;end
     else if(state1==READY)begin reader_active<=1;reader_bank<=1;read_position<=0;state1<=READING;end
    end
    if(accept_input&&!unusable)begin
     previous_capture<=sample_capture_id;previous_raw<=sample_raw_index;previous_config<=sample_config_id;have_input<=1;
     hop_position<=hop_position+1'b1;
     if(state0==FILLING)begin
      flags0<=flags0|effective_flags;
      if(write0==4095)state0<=READY;else write0<=write0+1'b1;
     end
     if(state1==FILLING)begin
      flags1<=flags1|effective_flags;
      if(write1==4095)state1<=READY;else write1<=write1+1'b1;
     end
     if(reserve_candidate)begin
      if(!FIXED_CAPTURE_GRID)candidate_count<=candidate_count+1'b1;
      history_gap<=0;
      // A final output handshake is a real release and can reserve the same
      // bank at this exact hop; no delayed arbitrary-position reconstruction.
      if(state0==FREE||release0)begin
       state0<=FILLING;write0<=1;capture0<=sample_capture_id;first0<=sample_raw_index;
       seq0<=FIXED_CAPTURE_GRID?grid_seq:candidate_count;config0<=sample_config_id;flags0<=effective_flags;
      end else if(state1==FREE||release1)begin
       state1<=FILLING;write1<=1;capture1<=sample_capture_id;first1<=sample_raw_index;
       seq1<=FIXED_CAPTURE_GRID?grid_seq:candidate_count;config1<=sample_config_id;flags1<=effective_flags;
      end
     end
    end
   end
  end
 end
 // First candidate metadata is captured at the producer's real grid binding,
 // before bank availability/publication. Only global reset or a newer capture
 // replaces it; local history/DMA recovery never relabels the first candidate.
 always@(posedge clk)begin
  if(rst)begin grid_meta_valid<=0;grid_meta_capture<=0;grid_meta_config<=0;grid_meta_seq<=0;grid_meta_raw<=0;end
  else if((FIXED_CAPTURE_GRID&&accept_input&&!unusable&&(!grid_valid||new_capture_good))&&(!grid_meta_valid||sample_capture_id>grid_meta_capture))begin
   grid_meta_valid<=1;grid_meta_capture<=sample_capture_id;grid_meta_config<=sample_config_id;
   grid_meta_seq<=candidate_count;grid_meta_raw<=sample_raw_index;
  end
 end
endmodule
