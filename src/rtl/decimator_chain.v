// 48 kHz -> 8 kHz -> 2 kHz -> 1 kHz. Histories and phase restart on gaps.
// Complete output m anchors are origin+48*m+47.
module decimator_chain #(
 parameter STAGE1_FILE="../../generated/fir_stage1.mem",
 parameter STAGE2_FILE="../../generated/fir_stage2.mem",
 parameter STAGE3_FILE="../../generated/fir_stage3.mem"
)(
 input wire clk,rst,clear,
 input wire s_valid,output wire s_ready,
 input wire signed [23:0] s_data,
 input wire [63:0] s_capture_id,s_raw_anchor,
 input wire [7:0] s_flags,
 output wire m_valid,input wire m_ready,
 output wire signed [23:0] m_data,
 output wire [63:0] m_capture_id,m_raw_anchor,
 output wire [7:0] m_flags,
 output wire m_warmup,
 output wire [63:0] saturation_count,
 output wire [63:0] stage1_saturation_count,stage2_saturation_count,stage3_saturation_count,
 output wire work_pending
);
 wire v1,r1,v2,r2,v3,ingress_ready;
 wire pending1,pending2,pending3;
 assign work_pending=pending1||pending2||pending3;
 wire signed [23:0] d1,d2;
 wire [63:0] c1,a1,c2,a2,sat1,sat2,sat3;
 wire [7:0] f1,f2,f3;
 reg have_previous;
 reg [63:0] previous_capture,previous_anchor;
 reg [5:0] output_index;
 reg gap_prepared;
 reg have_source_previous;
 reg [63:0] source_previous_capture,source_previous_anchor;
 wire source_boundary=s_flags[2] || (have_source_previous &&
  (s_capture_id!=source_previous_capture || s_raw_anchor!=source_previous_anchor+64'd1));
 // Detect only at raw ingress. ORed GAP flags in later FIR windows are data,
 // and must never repeatedly clear downstream history. This clear pulse is
 // independent of ready, so connecting a valid GAP introduces no ready loop.
 wire boundary_wait=s_valid && source_boundary && !gap_prepared;
 // A published valid beat is an irrevocable ready/valid offer. Keep it and
 // its metadata stable until consumed; only unpublished partial work clears.
 wire gap_clear=boundary_wait && !v3;
 wire pipeline_clear=clear || gap_clear;
 assign s_ready=ingress_ready && !pipeline_clear && !boundary_wait;
 assign m_valid=v3 && !pipeline_clear;
 wire discontinuity=!have_previous || m_capture_id!=previous_capture || m_raw_anchor!=previous_anchor+64'd48;
 assign m_warmup=discontinuity || output_index<34;
 assign m_flags=f3|(m_warmup ? 8'h02 : 8'h00);
 assign saturation_count=sat1+sat2+sat3;
 assign stage1_saturation_count=sat1;
 assign stage2_saturation_count=sat2;
 assign stage3_saturation_count=sat3;
 fir_decimator #(.TAPS(67),.DECIMATION(6),.ANCHOR_STEP(1),.COEFF_FILE(STAGE1_FILE)) stage1
  (.clk(clk),.rst(rst),.clear(pipeline_clear),.s_valid(s_valid&&!pipeline_clear&&!boundary_wait),.s_ready(ingress_ready),.s_data(s_data),
   .s_capture_id(s_capture_id),.s_raw_anchor(s_raw_anchor),.s_flags(s_flags),
   .m_valid(v1),.m_ready(r1),.m_data(d1),.m_capture_id(c1),.m_raw_anchor(a1),.m_flags(f1),.saturation_count(sat1),.work_pending(pending1));
 fir_decimator #(.TAPS(59),.DECIMATION(4),.ANCHOR_STEP(6),.COEFF_FILE(STAGE2_FILE)) stage2
  (.clk(clk),.rst(rst),.clear(pipeline_clear),.s_valid(v1),.s_ready(r1),.s_data(d1),
   .s_capture_id(c1),.s_raw_anchor(a1),.s_flags(f1),
   .m_valid(v2),.m_ready(r2),.m_data(d2),.m_capture_id(c2),.m_raw_anchor(a2),.m_flags(f2),.saturation_count(sat2),.work_pending(pending2));
 fir_decimator #(.TAPS(53),.DECIMATION(2),.ANCHOR_STEP(24),.COEFF_FILE(STAGE3_FILE)) stage3
  (.clk(clk),.rst(rst),.clear(pipeline_clear),.s_valid(v2),.s_ready(r2),.s_data(d2),
   .s_capture_id(c2),.s_raw_anchor(a2),.s_flags(f2),
   .m_valid(v3),.m_ready(m_ready&&!pipeline_clear),.m_data(m_data),.m_capture_id(m_capture_id),.m_raw_anchor(m_raw_anchor),.m_flags(f3),.saturation_count(sat3),.work_pending(pending3));
 always @(posedge clk) begin
  if(rst||clear) gap_prepared<=0;
  else if(gap_clear) gap_prepared<=1;
  else if(s_valid&&s_ready) gap_prepared<=0;
  if(rst||pipeline_clear) begin
   have_source_previous<=0; source_previous_capture<=0; source_previous_anchor<=0;
  end else if(s_valid&&s_ready) begin
   have_source_previous<=1; source_previous_capture<=s_capture_id; source_previous_anchor<=s_raw_anchor;
  end
 end
 always @(posedge clk) begin
  if(rst||pipeline_clear) begin have_previous<=0; previous_capture<=0; previous_anchor<=0; output_index<=0; end
  else if(m_valid&&m_ready) begin
   have_previous<=1; previous_capture<=m_capture_id; previous_anchor<=m_raw_anchor;
   if(discontinuity) output_index<=1;
   else if(output_index<34) output_index<=output_index+1'b1;
  end
 end
endmodule
