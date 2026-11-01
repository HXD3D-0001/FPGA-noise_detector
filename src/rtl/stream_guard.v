// Combinational forwarding preserves the upstream valid/ready contract.
// history_reset is qualified by the accepted beat, so a stalled gap resets once.
module stream_guard(input wire clk,rst,enable,
 input wire[63:0] expected_capture_id,input wire s_valid,output wire s_ready,
 input wire signed[23:0]s_data,input wire[63:0]s_capture_id,s_raw_index,input wire[7:0]s_flags,
 output wire m_valid,input wire m_ready,output wire signed[23:0]m_data,
 output wire[63:0]m_capture_id,m_raw_index,output wire[7:0]m_flags,
 output wire history_reset,output reg[63:0]gap_count,stale_count);
 reg have_previous;reg[63:0]previous_capture,previous_index;
 wire stale=s_capture_id!=expected_capture_id;
 wire discontinuity=s_flags[2] || (have_previous &&
  (s_capture_id!=previous_capture || previous_index==64'hffffffffffffffff || s_raw_index!=previous_index+1'b1));
 assign s_ready=enable && (stale || m_ready);
 assign m_valid=enable && s_valid && !stale;
 assign m_data=s_data;assign m_capture_id=s_capture_id;assign m_raw_index=s_raw_index;
 assign m_flags=s_flags | (discontinuity ? 8'h04 : 8'h00);
 assign history_reset=m_valid&&m_ready&&discontinuity;
 always @(posedge clk or posedge rst) begin
  if(rst)begin have_previous<=0;previous_capture<=0;previous_index<=0;gap_count<=0;stale_count<=0;end
  else if(!enable)have_previous<=0;
  else if(s_valid&&s_ready) begin
   if(stale)stale_count<=stale_count+1'b1;
   else begin
    have_previous<=1;previous_capture<=s_capture_id;previous_index<=s_raw_index;
    if(discontinuity)gap_count<=gap_count+1'b1;
   end
  end
 end
endmodule
