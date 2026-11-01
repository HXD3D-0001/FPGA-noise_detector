// 32-bit I2S slot receiver, sampled on BCK rising edges. At a LR transition
// the sampled bit is the delay bit; the following 24 bits are MSB first.
// Publish only at the NEXT LR transition, after verifying the whole slot.
// capture/channel changes require the CDC request/ACK barrier. origin_raw is
// a boot-time offset, stable until counter_rst. rst resets receiver alignment;
// counter_rst alone resets physical sequence and lifetime diagnostic counters.
module i2s_rx(input wire bclk,rst,counter_rst,enable,input wire[63:0]capture_id,origin_raw,
 input wire channel_select,lrck,sdata,output reg m_valid,input wire m_ready,
 output reg signed[23:0]m_data,output reg[63:0]m_capture_id,m_raw_index,
 output reg[7:0]m_flags,output reg[63:0]adc_count,drop_count,format_error_count,clip_count,
 output reg slot_aligned,output reg[63:0]good_slot_count);
 reg synced,observed_lr,previous_lr;reg[6:0]slot_count;reg[23:0]shift;
 reg pending_gap,pending_format;
 wire available=!m_valid||m_ready;
 wire rail=shift==24'h7fffff||shift==24'h800000;
 reg counter_observed_lr,counter_previous_lr;
 wire transition=synced&&lrck!=previous_lr;
 wire selected_valid_slot=!rst&&enable&&transition&&previous_lr==channel_select&&slot_count==32;
 // Physical LR tracking survives local FIFO/capture resets and TEST mode.
 // A selected slot candidate advances even if malformed or input is disabled.
 always@(posedge bclk or posedge counter_rst)begin
  if(counter_rst)begin
   counter_observed_lr<=0;counter_previous_lr<=0;adc_count<=0;drop_count<=0;format_error_count<=0;clip_count<=0;good_slot_count<=0;
  end else begin
   counter_observed_lr<=1;counter_previous_lr<=lrck;
   if(counter_observed_lr&&lrck!=counter_previous_lr&&counter_previous_lr==channel_select)adc_count<=adc_count+1'b1;
   if(!rst&&enable&&transition&&slot_count!=32)format_error_count<=format_error_count+1'b1;
   if(selected_valid_slot)good_slot_count<=good_slot_count+1'b1;
   if(selected_valid_slot&&rail)clip_count<=clip_count+1'b1;
   if(selected_valid_slot&&!available)drop_count<=drop_count+1'b1;
  end
 end
 always @(posedge bclk or posedge rst)begin
  if(rst)begin
   synced<=0;observed_lr<=0;previous_lr<=0;slot_count<=0;shift<=0;pending_gap<=0;pending_format<=0;slot_aligned<=0;
   m_valid<=0;m_data<=0;m_capture_id<=0;m_raw_index<=0;m_flags<=0;
  end else begin
   if(m_valid&&m_ready)m_valid<=0;
   if(!enable)begin synced<=0;observed_lr<=0;slot_count<=0;slot_aligned<=0;end
   else if(!synced)begin
    previous_lr<=lrck;
    // Acquire a real transition rather than treating an arbitrary reset phase
    // as the start of a complete slot.
    observed_lr<=1;
    if(observed_lr&&lrck!=previous_lr)begin synced<=1;slot_count<=1;shift<=0;end
   end else if(lrck!=previous_lr)begin
    previous_lr<=lrck;slot_count<=1;shift<=0;
    if(slot_count!=32)begin pending_gap<=1;pending_format<=1;slot_aligned<=0;end
    if(previous_lr==channel_select)begin
     if(slot_count==32)begin
      slot_aligned<=1;
      if(available)begin
       m_valid<=1;m_data<=shift;m_capture_id<=capture_id;m_raw_index<=origin_raw+adc_count;
       m_flags<=8'h01|(pending_gap ? 8'h04 : 0)|(pending_format ? 8'h10 : 0)|(rail ? 8'h08 : 0);
       pending_gap<=0;pending_format<=0;
      end else begin pending_gap<=1;end
     end
    end
   end else begin
    if(slot_count<127)slot_count<=slot_count+1'b1;
    if(slot_count>=1&&slot_count<=24)shift<={shift[22:0],sdata};
   end
  end
 end
endmodule
