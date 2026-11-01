// Signed24 FIR with serial DSP multiplication and synchronous circular history.
// Clear/gaps invalidate old RAM entries by count, preserving zero-padding.
// Add coefficient ROM files as Vivado memory sources.
module fir_decimator #(
 parameter integer TAPS=67, DECIMATION=6, ANCHOR_STEP=1,
 parameter COEFF_FILE="../../generated/fir_stage1.mem"
)(
 input wire clk,rst,clear,
 input wire s_valid, output wire s_ready,
 input wire signed [23:0] s_data,
 input wire [63:0] s_capture_id,s_raw_anchor,
 input wire [7:0] s_flags,
 output reg m_valid, input wire m_ready,
 output reg signed [23:0] m_data,
 output reg [63:0] m_capture_id,m_raw_anchor,
 output reg [7:0] m_flags,
 output reg [63:0] saturation_count,
 output wire work_pending
);
 localparam IDLE=0,FETCH=1,MULTIPLY=2,ADD=3,ROUND=4;
 localparam integer ADDRESS_BITS=(TAPS>1) ? $clog2(TAPS) : 1;
 localparam integer HISTORY_DEPTH=1<<ADDRESS_BITS;
 localparam integer PHASE_BITS=(DECIMATION>1) ? $clog2(DECIMATION+1) : 1;
 reg [2:0] state;
 (* rom_style="block" *) reg signed [17:0] coefficients[0:TAPS-1];
 (* ram_style="block" *) reg [31:0] history[0:HISTORY_DEPTH-1];
 reg [31:0] history_read;
 reg signed [17:0] coefficient_read;
 reg [ADDRESS_BITS-1:0] write_address,read_address,coefficient_address,tap;
 reg [ADDRESS_BITS:0] history_count;
 reg [PHASE_BITS-1:0] phase;
 wire signed [23:0] sample_read=(tap<history_count) ? $signed(history_read[23:0]) : 24'sd0;
 reg signed [41:0] product;
 reg signed [55:0] accumulator;
 reg [7:0] window_flags;
 reg have_previous;
 reg [63:0] previous_capture,previous_anchor;
 reg [63:0] pending_capture,pending_anchor;
 wire discontinuity=have_previous && ((s_capture_id!=previous_capture) ||
                                     (s_raw_anchor!=previous_anchor+ANCHOR_STEP));
 wire signed [55:0] product_extended={{14{product[41]}},product};
 wire [55:0] magnitude=accumulator[55] ? -accumulator : accumulator;
 wire [55:0] rounded_magnitude=(magnitude+56'd65536)>>17;
 wire signed [55:0] rounded_value=accumulator[55] ? -$signed(rounded_magnitude) : $signed(rounded_magnitude);
 assign s_ready=!rst && !clear && state==IDLE && (!m_valid || m_ready);
 assign work_pending=!rst&&!clear&&(state!=IDLE||m_valid);
 initial $readmemh(COEFF_FILE,coefficients);
 // No reset on either memory port: invalid entries are masked before use.
 always @(posedge clk) begin
  if(s_valid && s_ready) history[write_address]<={s_flags,s_data};
  history_read<=history[read_address];
  coefficient_read<=coefficients[coefficient_address];
 end
 always @(posedge clk) begin
  if(rst || clear) begin
   state<=IDLE; m_valid<=0; m_data<=0; m_capture_id<=0; m_raw_anchor<=0; m_flags<=0;
   accumulator<=0; product<=0; window_flags<=0; have_previous<=0;
   previous_capture<=0; previous_anchor<=0; pending_capture<=0; pending_anchor<=0;
   phase<=0; tap<=0; write_address<=0; read_address<=0; coefficient_address<=0; history_count<=0;
   if(rst) saturation_count<=0;
  end else begin
   if(m_valid && m_ready) m_valid<=0;
   case(state)
    IDLE: if(s_valid && s_ready) begin
     write_address<=write_address+1'b1;
     if(discontinuity) history_count<=1;
     else if(history_count<TAPS) history_count<=history_count+1'b1;
     have_previous<=1; previous_capture<=s_capture_id; previous_anchor<=s_raw_anchor;
     if((discontinuity && DECIMATION==1) || (!discontinuity && phase==DECIMATION-1)) begin
      phase<=0; accumulator<=0; window_flags<=0; tap<=0;
      pending_capture<=s_capture_id; pending_anchor<=s_raw_anchor;
      read_address<=write_address; coefficient_address<=0; state<=FETCH;
     end else phase<=discontinuity ? 1 : phase+1;
    end
    FETCH: state<=MULTIPLY;
    MULTIPLY: begin
     product<=sample_read*coefficient_read;
     if(tap<history_count) window_flags<=window_flags|history_read[31:24];
     // During ADD the synchronous ports prefetch the following tap.
     read_address<=read_address-1'b1; coefficient_address<=tap+1'b1; state<=ADD;
    end
    ADD: begin
     accumulator<=accumulator+product_extended;
     if(tap==TAPS-1) state<=ROUND;
     else begin tap<=tap+1'b1; state<=MULTIPLY; end
    end
    ROUND: begin
     m_capture_id<=pending_capture; m_raw_anchor<=pending_anchor;
     m_flags<=window_flags; m_valid<=1; state<=IDLE;
     if(rounded_value>56'sd8388607) begin
      m_data<=24'sh7fffff; m_flags<=window_flags|8'h08; saturation_count<=saturation_count+1'b1;
     end else if(rounded_value < -56'sd8388608) begin
      m_data<=24'sh800000; m_flags<=window_flags|8'h08; saturation_count<=saturation_count+1'b1;
     end else m_data<=rounded_value[23:0];
    end
   endcase
  end
 end
endmodule
