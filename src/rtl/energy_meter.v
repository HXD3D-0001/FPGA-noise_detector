// 256-point centered integer energy, stride48 and hop128. This is the
// valid/ready record core; a multiword AXI latch/pop adapter is separate.
module energy_meter #(parameter FIFO_DEPTH=64,ENABLE_RECORD_FLUSH=0)(
 input wire clk,rst,clear,s_valid,output wire s_ready,
 input wire fifo_flush,output wire[31:0]occupancy,output reg[63:0]flush_count,
 input wire signed[23:0]s_data,input wire[63:0]s_capture_id,s_raw_index,
 input wire[31:0]s_config_id,input wire[7:0]s_flags,
 output reg grid_meta_valid,output reg[63:0]grid_meta_capture,grid_meta_seq,grid_meta_raw,
 output reg[31:0]grid_meta_config,
 output wire m_valid,input wire m_ready,
 output wire[63:0]m_seq,m_capture_id,m_first_raw,m_energy,
 output wire[31:0]m_config_id,output wire[7:0]m_flags,
 output reg[63:0]candidate_count,drop_count,discontinuity_count,config_error_count,
 output reg[31:0]high_water);
 localparam FW=(FIFO_DEPTH<=1)?1:$clog2(FIFO_DEPTH);
 (* ram_style="block" *)reg[295:0]records[0:FIFO_DEPTH-1];
 reg[295:0]memory_head,bypass_head;reg bypass_active;
 reg[FW-1:0]rd,wr;reg[31:0]queue_count;
 reg pending_gap;
 (* ram_style="distributed" *)reg signed[23:0]history[0:255];
 (* ram_style="distributed" *)reg[7:0]flag_history[0:255];
 reg[8:0]flag_count[0:7];reg[7:0]position,hop;reg[8:0]history_count;
 reg signed[32:0]sum;reg[55:0]squares;
 reg have_previous,poisoned,history_gap;reg[63:0]previous_capture,previous_raw,poison_capture;
 reg[31:0]previous_config;
 reg contract_valid;reg[63:0]contract_capture;reg[31:0]contract_config;
 wire flushing=ENABLE_RECORD_FLUSH&&fifo_flush;
 wire pop=m_valid&&m_ready;
 wire metadata_invalid=s_capture_id==0||s_config_id==0;
 wire stale=contract_valid&&s_capture_id<contract_capture;
 wire newer=!contract_valid||s_capture_id>contract_capture;
 wire config_error=!stale&&contract_valid&&s_capture_id==contract_capture&&s_config_id!=contract_config&&!poisoned;
 wire invalid=metadata_invalid||!s_flags[0]||(|s_flags[7:6]);
 wire blocked=(poisoned&&s_capture_id<=poison_capture)||config_error;
 wire boundary=s_flags[2]||(have_previous&&(s_capture_id!=previous_capture||
  s_config_id!=previous_config||previous_raw>64'hffffffffffffffcf||s_raw_index!=previous_raw+64'd48));
 wire restart=boundary||history_count==0;
 wire[7:0]effective_flags=s_flags|((boundary||history_gap)?8'h04:8'h00);
 wire candidate=!restart&&(history_count==255||(history_count==256&&hop==127));
 wire signed[47:0]new_square=$signed(s_data)*$signed(s_data);
 wire signed[47:0]old_square=$signed(history[position])*$signed(history[position]);
 reg signed[32:0]next_sum;reg[55:0]next_squares;
 reg[8:0]next_flag_count[0:7];reg[7:0]window_flags;
 reg signed[65:0]sum_square;reg[65:0]energy_wide;
 wire[7:0]next_position=position+1'b1;
 // A candidate includes this accepted beat. The continuity checks ensure all
 // 256 members have stride48, including overflow rejection before publication.
 // Their first anchor therefore needs no per-sample raw-index memory.
 wire[63:0]first_raw=s_raw_index-64'd12240;
 wire history_write=s_valid&&s_ready&&!stale&&!invalid&&!blocked&&!s_flags[1];
 wire[7:0]history_write_address=restart?8'd0:position;
 wire record_write=history_write&&candidate&&(queue_count<FIFO_DEPTH||pop);
 wire[295:0]record_word={candidate_count,s_capture_id,first_raw,energy_wide[63:0],s_config_id,window_flags|(pending_gap?8'h04:8'h00)};
 wire[FW-1:0]next_rd=(rd==FIFO_DEPTH-1)?0:rd+1'b1;
 wire head_bypass=record_write&&(queue_count==0||(pop&&queue_count==1));
 wire head_read=pop&&queue_count>1;
 integer b,k;
 assign s_ready=!rst&&!clear&&!flushing;
 assign m_valid=!rst&&!flushing&&queue_count!=0;
 assign occupancy=queue_count;
 assign {m_seq,m_capture_id,m_first_raw,m_energy,m_config_id,m_flags}=bypass_active?bypass_head:memory_head;
 // Storage ports deliberately have no async reset or array reset. Ownership
 // resets hide stale words. History retains its asynchronous distributed read;
 // records use a synchronous successor read to infer block RAM.
 always @(posedge clk)begin
  if(history_write)begin
   history[history_write_address]<=s_data;
   flag_history[history_write_address]<=effective_flags;
  end
  if(record_write)records[wr]<=record_word;
  // Read the successor on the POP edge: its registered value becomes the
  // next offered head immediately, preserving continuous valid without bubbles.
  // Empty PUSH and one-item POP+PUSH use bypass, avoiding read/write collisions.
  if(head_read)memory_head<=records[next_rd];
  if(head_bypass)bypass_head<=record_word;
 end
 always @(posedge clk or posedge rst)begin
  if(rst)bypass_active<=0;
  else if(flushing)bypass_active<=0;
  else if(head_bypass)bypass_active<=1;
  else if(head_read)bypass_active<=0;
 end
 always @*begin
  if(restart)begin
   next_sum={{9{s_data[23]}},s_data};next_squares={8'b0,new_square};
  end else begin
   next_sum=sum+$signed(s_data);
   next_squares=squares+{8'b0,new_square};
   if(history_count==256)begin
    next_sum=next_sum-$signed(history[position]);next_squares=next_squares-{8'b0,old_square};
   end
  end
  window_flags=0;
  for(k=0;k<8;k=k+1)begin
   if(restart)next_flag_count[k]=effective_flags[k]?9'd1:9'd0;
   else begin
    next_flag_count[k]=flag_count[k]+(effective_flags[k]?9'd1:9'd0);
    if(history_count==256)next_flag_count[k]=next_flag_count[k]-(flag_history[position][k]?9'd1:9'd0);
   end
   window_flags[k]=next_flag_count[k]!=0;
  end
  // Both terms are represented with 66 bits: no 64-bit multiply/shift truncation.
  sum_square=$signed(next_sum)*$signed(next_sum);
  energy_wide=({10'b0,next_squares}<<8)-$unsigned(sum_square);
 end
 always @(posedge clk or posedge rst)begin
  if(rst)begin
   rd<=0;wr<=0;queue_count<=0;pending_gap<=0;candidate_count<=0;drop_count<=0;high_water<=0;flush_count<=0;
   discontinuity_count<=0;config_error_count<=0;
   have_previous<=0;poisoned<=0;history_gap<=0;previous_capture<=0;previous_raw<=0;previous_config<=0;poison_capture<=0;
   contract_valid<=0;contract_capture<=0;contract_config<=0;
   history_count<=0;position<=0;hop<=0;sum<=0;squares<=0;
   for(b=0;b<8;b=b+1)flag_count[b]<=0;
  end else begin
   if(flushing)begin
    // Only the coordinator pulses this after stopped source and matching DMA
    // reset completion. Flush wins simultaneous POP and candidate publication.
    // An empty boot/STOP reset loses nothing; retain only real loss evidence.
    flush_count<=flush_count+queue_count;rd<=0;wr<=0;queue_count<=0;pending_gap<=pending_gap||(queue_count!=0);
    history_count<=0;position<=0;hop<=0;sum<=0;squares<=0;have_previous<=0;history_gap<=history_gap||(history_count!=0);
    for(b=0;b<8;b=b+1)flag_count[b]<=0;
   end else begin
   // clear never cancels a published record or lifetime counter.
   if(pop)begin rd<=(rd==FIFO_DEPTH-1)?0:rd+1'b1;queue_count<=queue_count-1'b1;end
   if(clear)begin
    // Retain capture/config contract state across local history recovery.
    history_gap<=0;history_count<=0;position<=0;hop<=0;sum<=0;squares<=0;
    for(b=0;b<8;b=b+1)flag_count[b]<=0;
   end else if(s_valid&&s_ready&&stale)begin
    // A stale identity is rejected without disturbing the current valid window
    // or changing the highest canonical capture/configuration barrier.
    discontinuity_count<=discontinuity_count+1'b1;
   end else if(s_valid&&s_ready)begin
    if(!invalid&&newer)begin
     contract_valid<=1;contract_capture<=s_capture_id;contract_config<=s_config_id;
     poisoned<=0;
    end
    previous_capture<=s_capture_id;previous_raw<=s_raw_index;previous_config<=s_config_id;have_previous<=1;
    if(config_error)begin
     config_error_count<=config_error_count+1'b1;poisoned<=1;
     if(!poisoned||s_capture_id>poison_capture)poison_capture<=s_capture_id;
    end
    if(invalid||blocked||s_flags[1])begin
     history_count<=0;position<=0;hop<=0;sum<=0;squares<=0;
     for(b=0;b<8;b=b+1)flag_count[b]<=0;
     if(invalid)begin discontinuity_count<=discontinuity_count+1'b1;history_gap<=1;end
    end else begin
     if(boundary)discontinuity_count<=discontinuity_count+1'b1;
     history_gap<=0;sum<=next_sum;squares<=next_squares;
     for(b=0;b<8;b=b+1)flag_count[b]<=next_flag_count[b];
     if(restart)begin
      history_count<=1;position<=1;hop<=0;
     end else begin
      position<=next_position;
      if(history_count<256)history_count<=history_count+1'b1;
      if(candidate)begin
       hop<=0;candidate_count<=candidate_count+1'b1;
       if(queue_count<FIFO_DEPTH||pop)begin
        wr<=(wr==FIFO_DEPTH-1)?0:wr+1'b1;queue_count<=queue_count+(pop?0:1);
        if(queue_count+(pop?0:1)>high_water)high_water<=queue_count+(pop?0:1);
        pending_gap<=0;
       end else begin drop_count<=drop_count+1'b1;pending_gap<=1;end
      end else if(history_count==256)hop<=hop+1'b1;
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
  else if((history_write)&&(!grid_meta_valid||s_capture_id>grid_meta_capture))begin
   grid_meta_valid<=1;grid_meta_capture<=s_capture_id;grid_meta_config<=s_config_id;
   grid_meta_seq<=candidate_count;grid_meta_raw<=s_raw_index;
  end
 end
endmodule
