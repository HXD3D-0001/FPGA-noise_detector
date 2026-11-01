// DAT1 lifecycle owner. Commands: 1 drain, 2 reset both, 3 spectrum FLOW,
// 4 PCM FLOW. Does not gate the source: drain/reset require source_stopped.
// Proof requests are held through exact DMA epoch/mask ACK; timeout never frees.
module capture_ownership_service #(parameter integer TIMEOUT_CYCLES=5000000)(
 input wire clk,rst,capture_start,source_stopped,drained,input wire[31:0]energy_occupancy,
 input wire command_req,input wire[2:0]command_kind,input wire[31:0]command_seq,
 output reg command_ack,command_error,command_timed_out,output reg[31:0]command_ack_seq,
 output reg[3:0]command_reason,output wire busy,output reg quarantined,dma_idle,
 output reg[31:0]dma_request_seq,output reg[1:0]dma_request_mask,output reg dma_request_reset,dma_request_resume,
 input wire dma_ack_valid,input wire[31:0]dma_ack_seq,input wire[1:0]dma_ack_mask,
 output reg spectrum_flush_req,dma_spectrum_reset_done,dma_pcm_abort,clear_energy_records,
 output reg[31:0]dma_spectrum_reset_seq,
 input wire spectrum_flush_pending,input wire[31:0]spectrum_flush_request_seq,
 output reg spectrum_output_hold,pcm_output_hold
);
 localparam IDLE=0,DRAIN=1,PROOF=2,APPLY=3,FINISH=4;
 reg[2:0]state,kind;reg[31:0]ticks,op_tag,seen_tag,max_tag;reg seen,proof_seen,violation;
 reg[2:0]settle;
 wire fresh=command_req&&(!seen||command_seq!=seen_tag);
 wire match=dma_ack_valid&&dma_request_mask!=0&&dma_ack_seq==dma_request_seq&&dma_ack_mask==dma_request_mask;
 wire timed=ticks>=TIMEOUT_CYCLES-1;
 assign busy=state!=IDLE;
 task request_proof;input[1:0]mask;input reset_request,resume;begin
  dma_request_seq<=dma_request_seq+1'b1;dma_request_mask<=mask;
  dma_request_reset<=reset_request;dma_request_resume<=resume;proof_seen<=0;state<=PROOF;
 end endtask
 always@(posedge clk or posedge rst)begin
  if(rst)begin
   state<=IDLE;kind<=0;ticks<=0;op_tag<=0;seen_tag<=0;max_tag<=0;seen<=0;proof_seen<=0;violation<=0;settle<=0;
   command_ack<=0;command_error<=0;command_timed_out<=0;command_ack_seq<=0;command_reason<=0;
   quarantined<=0;dma_idle<=0;dma_request_seq<=0;dma_request_mask<=0;dma_request_reset<=0;dma_request_resume<=0;
   spectrum_flush_req<=0;dma_spectrum_reset_done<=0;dma_pcm_abort<=0;clear_energy_records<=0;dma_spectrum_reset_seq<=0;
   spectrum_output_hold<=0;pcm_output_hold<=0;
  end else begin
   command_ack<=0;spectrum_flush_req<=0;dma_spectrum_reset_done<=0;dma_pcm_abort<=0;clear_energy_records<=0;
   if(capture_start)begin
    dma_idle<=0;
    if(state!=IDLE)begin violation<=1;quarantined<=1;command_reason<=4;end
   end
   if(state!=IDLE)begin
    if(ticks<TIMEOUT_CYCLES)ticks<=ticks+1'b1;
    if(timed)begin command_timed_out<=1;quarantined<=1;end
   end
   case(state)
    IDLE:if(fresh)begin
     seen<=1;seen_tag<=command_seq;command_ack_seq<=command_seq;command_error<=0;command_reason<=0;command_timed_out<=0;
     if(command_seq==0||(seen&&command_seq<=max_tag))begin command_ack<=1;command_error<=1;command_reason<=1;end
     else begin
      max_tag<=command_seq;
      if(command_kind<1||command_kind>4)begin command_ack<=1;command_error<=1;command_reason<=2;end
      else if(capture_start||((command_kind==1||command_kind==2)&&!source_stopped))begin command_ack<=1;command_error<=1;command_reason<=3;end
      else if(dma_request_seq==32'hffffffff)begin command_ack<=1;command_error<=1;command_reason<=5;end
      else begin
       kind<=command_kind;op_tag<=command_seq;ticks<=0;dma_idle<=0;violation<=0;quarantined<=0;
       if(command_kind==1)state<=DRAIN;
       else begin
        spectrum_output_hold<=command_kind!=4;pcm_output_hold<=command_kind!=3;
        spectrum_flush_req<=command_kind!=4;
        request_proof(command_kind==3?2'd1:(command_kind==4?2'd2:2'd3),1'b1,command_kind>=3);
       end
      end
     end
    end
    DRAIN:if(!source_stopped)begin violation<=1;quarantined<=1;command_reason<=4;end
     else if(!violation&&drained&&energy_occupancy==0)request_proof(2'd3,1'b0,1'b0);
    PROOF:begin
     if(match)proof_seen<=1;
     if((kind==1||kind==2)&&!source_stopped)begin violation<=1;quarantined<=1;command_reason<=4;end
     if(proof_seen&&!violation&&!capture_start)begin
      if(kind==1)begin dma_request_mask<=0;state<=FINISH;settle<=0;end
      else if(!dma_request_mask[0]||spectrum_flush_pending)state<=APPLY;
     end
    end
    APPLY:if(!violation&&!capture_start)begin
     dma_spectrum_reset_done<=dma_request_mask[0];dma_spectrum_reset_seq<=spectrum_flush_request_seq;
     dma_pcm_abort<=dma_request_mask[1];clear_energy_records<=kind==2;
     dma_request_mask<=0;state<=FINISH;settle<=0;
    end
    FINISH:begin
     if(settle<3)settle<=settle+1'b1;
     else if(!violation&&!capture_start&&(kind>=3||source_stopped)&&
       ((kind==3||kind==2)?!spectrum_flush_pending:1'b1)&&
       (kind>=3||(drained&&energy_occupancy==0)))begin
      spectrum_output_hold<=0;pcm_output_hold<=0;dma_idle<=(kind<=2);
      command_ack<=1;command_ack_seq<=op_tag;command_error<=command_timed_out;
      command_reason<=command_timed_out?4'd6:4'd0;quarantined<=0;state<=IDLE;
     end
    end
    default:begin quarantined<=1;violation<=1;end
   endcase
  end
 end
 initial if(TIMEOUT_CYCLES<8)$error("ownership timeout must be >=8");
endmodule
