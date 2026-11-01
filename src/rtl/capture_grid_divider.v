// Exact u64 capture grid. Normal physical samples retain one-cycle throughput.
// A discontinuity uses 64 restoring division cycles, then 64 serial addition
// and 64 serial subtraction cycles. All metadata remains held until completion.
module capture_grid_divider #(parameter integer DIVISOR=48,HOP_SHIFT=8)(
 input wire clk,rst,cancel,start,new_anchor,sequential,commit,
 input wire[63:0]delta,first_seq,candidate_count,
 output wire ready,valid,held,output wire[63:0]quotient,
 output wire remainder_nonzero,output wire[63:0]grid_seq,grid_missed,
 output wire grid_bad);
 generate if(DIVISOR==1)begin:g_stride_one
  wire[63:0]index=(new_anchor?64'd0:delta)>>HOP_SHIFT;
  wire due=((new_anchor?64'd0:delta)&((64'd1<<HOP_SHIFT)-1))==0;
  assign ready=!rst&&!cancel;assign valid=start&&ready;assign held=0;
  assign quotient=new_anchor?64'd0:delta;assign remainder_nonzero=0;
  assign grid_seq=new_anchor?candidate_count:first_seq+index;
  assign grid_missed=grid_seq>=candidate_count?grid_seq-candidate_count+(due?64'd0:64'd1):64'd0;
  assign grid_bad=candidate_count==64'hffffffffffffffff||(!new_anchor&&(first_seq>64'hffffffffffffffff-index||grid_seq==64'hffffffffffffffff));
 end else begin:g_serial
  localparam integer RB=$clog2(DIVISOR);
  localparam[2:0]IDLE=0,DIVIDE=1,ADD=2,SUBTRACT=3,RESULT=4;
  reg[2:0]state;reg cached;reg[5:0]bit_count;
  // Caller grid context is stable while held; only the source delta is
  // transient. Reuse the two DIV shift registers as ADD/SUB operands.
  reg[63:0]numerator,partial_q,cache_q,cache_seq,arithmetic_shift;
  reg[RB-1:0]partial_r;
  reg[1:0]carry;reg result_bad;
  wire fast=new_anchor||(sequential&&cached);
  wire[63:0]fast_q=new_anchor?64'd0:cache_q+1'b1;
  wire fast_due=fast_q[HOP_SHIFT-1:0]==0;
  wire[63:0]fast_seq=new_anchor?candidate_count:cache_seq+fast_due;
  wire fast_bad=candidate_count==64'hffffffffffffffff||(!new_anchor&&(fast_seq==64'hffffffffffffffff||(fast_due&&cache_seq==64'hffffffffffffffff)));
  wire[RB:0]trial={partial_r,numerator[63]};
  wire subtract=trial>=DIVISOR;
  wire[RB:0]next_r=subtract?trial-DIVISOR:trial;
  wire[63:0]next_q={partial_q[62:0],subtract};
  wire[2:0]bit_sum={2'd0,numerator[0]}+{2'd0,partial_q[0]}+{1'b0,carry};
  wire[63:0]next_arithmetic={bit_sum[0],arithmetic_shift[63:1]};
  assign ready=!rst&&!cancel&&state==IDLE;
  assign valid=!rst&&!cancel&&(state==RESULT||(start&&ready&&fast));
  assign held=state!=IDLE;
  assign quotient=held?cache_q:fast_q;
  assign remainder_nonzero=held&&(partial_r!=0);
  assign grid_seq=held?cache_seq:fast_seq;
  assign grid_missed=held?arithmetic_shift:64'd0;
  assign grid_bad=held?result_bad:fast_bad;
  always@(posedge clk)begin
   if(rst)begin state<=IDLE;cached<=0;bit_count<=0;
    // Datapath contents have no meaning without cached or RESULT ownership.
    // Each slow phase initializes its operands before publishing its result.
   end else if(cancel)begin
    // A pending operation may have replaced its cache with a partial result.
    // IDLE local resets retain the last committed physical sample's cache.
    if(state!=IDLE)cached<=0;
    state<=IDLE;
   end else begin
    if(valid)begin
     cached<=commit;
     if(commit&&state==IDLE)begin cache_q<=fast_q;cache_seq<=fast_seq;end
    end
    case(state)
     IDLE:if(start&&!fast)begin
      state<=DIVIDE;numerator<=delta;partial_q<=0;partial_r<=0;bit_count<=0;
     end
     DIVIDE:begin
      numerator<={numerator[62:0],1'b0};partial_q<=next_q;partial_r<=next_r[RB-1:0];bit_count<=bit_count+1'b1;
      if(bit_count==63)begin
       cache_q<=next_q;numerator<=first_seq;partial_q<=next_q>>HOP_SHIFT;
       arithmetic_shift<=0;carry<=0;bit_count<=0;state<=ADD;
      end
     end
     ADD,SUBTRACT:begin
      numerator<=numerator>>1;partial_q<=partial_q>>1;arithmetic_shift<=next_arithmetic;carry<=bit_sum[2:1];bit_count<=bit_count+1'b1;
      if(bit_count==63)begin
       if(state==ADD)begin
        cache_seq<=next_arithmetic;
        result_bad<=(bit_sum[2:1]!=0)||next_arithmetic==64'hffffffffffffffff||candidate_count==64'hffffffffffffffff;
        numerator<=next_arithmetic;partial_q<=~candidate_count;arithmetic_shift<=0;
        carry<=cache_q[HOP_SHIFT-1:0]==0?2'd1:2'd2;bit_count<=0;state<=SUBTRACT;
       end else begin
        arithmetic_shift<=bit_sum[2:1]!=0?next_arithmetic:64'd0;state<=RESULT;
       end
      end
     end
     RESULT:state<=IDLE;
     default:state<=IDLE;
    endcase
   end
  end
 end endgenerate
 initial begin
  if(DIVISOR<=0)$error("grid divisor must be positive");
  if(HOP_SHIFT<1||HOP_SHIFT>63)$error("grid hop shift must be in 1..63");
 end
endmodule
