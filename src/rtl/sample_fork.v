// Three independently bounded sample branches. rst is boot reset; clear flushes queues only.
module sample_fork #(parameter DEPTH=16)(input wire clk,rst,clear,s_valid, output wire s_ready,input wire signed[23:0]s_data,input wire[63:0]s_capture_id,s_raw_index,input wire[7:0]s_flags,
output wire m0_valid,input wire m0_ready,output wire signed[23:0]m0_data,output wire[63:0]m0_capture_id,m0_raw_index,output wire[7:0]m0_flags,output wire[63:0]m0_drop_count,output wire[31:0]m0_high_water,
output wire m1_valid,input wire m1_ready,output wire signed[23:0]m1_data,output wire[63:0]m1_capture_id,m1_raw_index,output wire[7:0]m1_flags,output wire[63:0]m1_drop_count,output wire[31:0]m1_high_water,
output wire m2_valid,input wire m2_ready,output wire signed[23:0]m2_data,output wire[63:0]m2_capture_id,m2_raw_index,output wire[7:0]m2_flags,output wire[63:0]m2_drop_count,output wire[31:0]m2_high_water);
assign s_ready=!rst&&!clear;
sample_fork_queue #(.DEPTH(DEPTH)) branch0(.clk(clk),.rst(rst),.clear(clear),.push(s_valid&&s_ready),.word_in({s_capture_id,s_raw_index,s_flags,s_data}),.valid(m0_valid),.ready(m0_ready),.word_out({m0_capture_id,m0_raw_index,m0_flags,m0_data}),.drop_count(m0_drop_count),.high_water(m0_high_water));
sample_fork_queue #(.DEPTH(DEPTH)) branch1(.clk(clk),.rst(rst),.clear(clear),.push(s_valid&&s_ready),.word_in({s_capture_id,s_raw_index,s_flags,s_data}),.valid(m1_valid),.ready(m1_ready),.word_out({m1_capture_id,m1_raw_index,m1_flags,m1_data}),.drop_count(m1_drop_count),.high_water(m1_high_water));
sample_fork_queue #(.DEPTH(DEPTH)) branch2(.clk(clk),.rst(rst),.clear(clear),.push(s_valid&&s_ready),.word_in({s_capture_id,s_raw_index,s_flags,s_data}),.valid(m2_valid),.ready(m2_ready),.word_out({m2_capture_id,m2_raw_index,m2_flags,m2_data}),.drop_count(m2_drop_count),.high_water(m2_high_water));
endmodule

// One ownership queue per branch: drop newest, never edit an offered head.
module sample_fork_queue #(parameter DEPTH=16)(
 input wire clk,rst,clear,push,input wire[159:0]word_in,
 output wire valid,input wire ready,output wire[159:0]word_out,
 output reg[63:0]drop_count,output reg[31:0]high_water);
 localparam PW=(DEPTH<=1)?1:$clog2(DEPTH);
 localparam CW=(DEPTH<=1)?1:$clog2(DEPTH+1);
 (* ram_style="distributed" *)reg[159:0]memory[0:DEPTH-1];reg[PW-1:0]rd,wr;
 // Occupancy is bounded by storage depth; lifetime drop_count stays u64.
 reg[CW-1:0]count;reg pending_gap;
 wire pop=valid&&ready;
 wire retain=push&&(count<DEPTH||pop);
 assign valid=!rst&&!clear&&count!=0;
 assign word_out=memory[rd];
 // An offered head is changed only by a pop or explicit ownership clear.
 // Separate, reset-free write port keeps the small queues in distributed RAM.
 always @(posedge clk)
  if(!rst&&!clear&&retain)
   memory[wr]<={word_in[159:32],word_in[31:24]|(pending_gap?8'h04:8'h00),word_in[23:0]};
 always @(posedge clk or posedge rst)begin
  if(rst)begin rd<=0;wr<=0;count<=0;pending_gap<=0;drop_count<=0;high_water<=0;end
  else if(clear)begin rd<=0;wr<=0;count<=0;pending_gap<=0;end
  else begin
   if(pop)rd<=(rd==DEPTH-1)?0:rd+1'b1;
   if(retain)begin
    wr<=(wr==DEPTH-1)?0:wr+1'b1;pending_gap<=0;
    if(!pop && count+1>high_water)high_water<=count+1;
   end else if(push)begin drop_count<=drop_count+1'b1;pending_gap<=1;end
   case({retain,pop})2'b10:count<=count+1'b1;2'b01:count<=count-1'b1;default:count<=count;endcase
  end
 end
endmodule
