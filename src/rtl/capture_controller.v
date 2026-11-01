// Input capture lifecycle only. drain_done is the downstream packet/energy
// barrier; DMA ownership/recovery and PS control transactions live elsewhere.
module capture_controller #(
 parameter integer RESET_HOLD_CYCLES=8,ADC_WARMUP_CYCLES=100000000,TIMEOUT_CYCLES=250000000,DRAIN_TIMEOUT_CYCLES=5000000
)(input wire clk,rst,start_req,stop_req,requested_test_mode,
 input wire[63:0]next_capture_id,input wire drain_done,adc_reset_done,adc_ack_valid,adc_ack_toggle,
 input wire[63:0]adc_ack_capture_id,
 output wire running,test_enable,adc_enable,source_gate,fifo_reset,active_test_mode,
 output reg source_req_toggle,pattern_start,output reg[63:0]active_capture_id,
 output reg[3:0]state,fault_reason,output wire busy,output reg command_rejected,
 input wire source_fault,input wire[3:0]source_fault_reason);
 localparam STOPPED=0,RESET_HOLD=1,WAIT_RESET=2,WAIT_ACK=3,WARMUP=4,RUN=5,DRAIN=6,FAULT=7;
 reg active_test;reg[63:0]timer;
 assign active_test_mode=active_test;
 assign running=state==RUN;assign source_gate=running;
 assign test_enable=active_test&&(state==WARMUP||state==RUN);
 assign adc_enable=!active_test&&(state==WAIT_ACK||state==WARMUP||state==RUN);
 assign fifo_reset=active_test||state==STOPPED||state==RESET_HOLD||state==FAULT;
 assign busy=state!=STOPPED&&state!=RUN&&state!=FAULT;
 always@(posedge clk or posedge rst)begin
  if(rst)begin state<=STOPPED;active_test<=0;active_capture_id<=0;source_req_toggle<=0;pattern_start<=0;timer<=0;fault_reason<=0;command_rejected<=0;end
  else begin
   pattern_start<=0;command_rejected<=0;
   if(start_req&&(state!=STOPPED||stop_req||next_capture_id==0||next_capture_id<=active_capture_id))command_rejected<=1;
   if(source_fault&&(state==RUN||(!active_test&&(state==RESET_HOLD||state==WAIT_RESET||state==WAIT_ACK||state==WARMUP))))begin state<=FAULT;fault_reason<=source_fault_reason;timer<=0;end
   else if(stop_req&&state==RUN)begin state<=DRAIN;timer<=0;end
   else if(stop_req&&(state==RESET_HOLD||state==WAIT_RESET||state==WAIT_ACK||state==WARMUP||state==FAULT))begin state<=STOPPED;timer<=0;end
   else case(state)
    STOPPED:if(start_req&&!stop_req&&next_capture_id!=0&&next_capture_id>active_capture_id)begin
     active_capture_id<=next_capture_id;active_test<=requested_test_mode;fault_reason<=0;timer<=0;
     source_req_toggle<=~source_req_toggle;
     if(requested_test_mode)begin pattern_start<=1;state<=WARMUP;end
     else state<=RESET_HOLD;
    end
    RESET_HOLD:if(timer+1>=RESET_HOLD_CYCLES)begin state<=WAIT_RESET;timer<=0;end else timer<=timer+1'b1;
    WAIT_RESET:if(adc_reset_done)begin state<=WAIT_ACK;timer<=0;end
     else if(timer+1>=TIMEOUT_CYCLES)begin state<=FAULT;fault_reason<=1;end else timer<=timer+1'b1;
    WAIT_ACK:if(adc_ack_valid&&adc_ack_toggle==source_req_toggle&&adc_ack_capture_id==active_capture_id)begin state<=WARMUP;timer<=0;end
     else if(timer+1>=TIMEOUT_CYCLES)begin state<=FAULT;fault_reason<=2;end else timer<=timer+1'b1;
    WARMUP:if(active_test||timer+1>=ADC_WARMUP_CYCLES)begin state<=RUN;timer<=0;end else timer<=timer+1'b1;
    RUN:timer<=0;
    DRAIN:if(drain_done)begin state<=STOPPED;timer<=0;end
     else if(timer+1>=DRAIN_TIMEOUT_CYCLES)begin state<=FAULT;fault_reason<=3;end else timer<=timer+1'b1;
    default:state<=FAULT;
   endcase
  end
 end
endmodule
