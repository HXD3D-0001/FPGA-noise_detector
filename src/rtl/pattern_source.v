// Deterministic proc-domain source. start is legal only while disabled or after
// the downstream drain barrier; it discards the preceding capture's held word.
// origin_raw is the boot sequence seed, loaded on a clk edge while rst is high.
// Complete-system reset must span a clk rising edge. start changes the
// waveform phase/capture, preserving source sequence and cumulative counters.
module pattern_source #(
 parameter integer CLK_HZ=50000000, SAMPLE_HZ=48000,
 parameter SINE_FILE="../../generated/pattern_sine.mem",
 parameter integer SYNCHRONOUS_ROM=1
)(input wire clk,rst,enable,start,
 input wire [63:0] capture_id,origin_raw,input wire [1:0] pattern_id,
 output reg m_valid,input wire m_ready,output reg signed [23:0] m_data,
 output reg [63:0] m_capture_id,m_raw_index,output reg [7:0] m_flags,
 output reg [63:0] raw_count,drop_count);
 localparam integer PHASE_BITS=$clog2(64'd0+CLK_HZ+SAMPLE_HZ);
 reg [PHASE_BITS-1:0] phase;
 reg [63:0] seq,pattern_position; reg pending_gap;
 // Four phase accumulators advance on physical source ticks, including drops.
 // The ROM amplitude is 1/8 FS per component, leaving headroom for their sum.
 (* rom_style="block" *) reg signed[23:0]sine[0:1023];
 reg[31:0]tone0,tone1,tone2,tone3,noise;
 localparam[31:0]STEP0=(64'd4294967296*63+SAMPLE_HZ)/(2*SAMPLE_HZ);
 localparam[31:0]STEP1=(64'd4294967296*63+SAMPLE_HZ/2)/SAMPLE_HZ;
 localparam[31:0]STEP2=(64'd4294967296*125+SAMPLE_HZ/2)/SAMPLE_HZ;
 localparam[31:0]STEP3=(64'd4294967296*250+SAMPLE_HZ/2)/SAMPLE_HZ;
 wire signed[23:0]sine0,sine1,sine2,sine3;
 // At >=2 clocks per source tick, tones remain stable for the intervening
 // ROM-read edge. start/rst also leave a full read edge before the first tick.
 // Faster accelerated profiles retain asynchronous reads and exact old values.
 generate if(SYNCHRONOUS_ROM && CLK_HZ>=64'd2*SAMPLE_HZ)begin: synchronous_rom
  reg signed[23:0]word0,word1,word2,word3;
  always @(posedge clk)begin
   word0<=sine[tone0[31:22]];word1<=sine[tone1[31:22]];
   word2<=sine[tone2[31:22]];word3<=sine[tone3[31:22]];
  end
  assign sine0=word0;assign sine1=word1;assign sine2=word2;assign sine3=word3;
 end else begin: asynchronous_rom
  assign sine0=sine[tone0[31:22]];assign sine1=sine[tone1[31:22]];
  assign sine2=sine[tone2[31:22]];assign sine3=sine[tone3[31:22]];
 end endgenerate
 wire signed[25:0]mixed=$signed(sine0)+$signed(sine1)+$signed(sine2)+$signed(sine3)+($signed(noise[23:0])>>>10);
 wire [PHASE_BITS:0]phase_sum={1'b0,phase}+SAMPLE_HZ;
 wire tick = phase_sum >= CLK_HZ;
 wire available = !m_valid || m_ready;
 initial begin
  if(SAMPLE_HZ<=0||CLK_HZ<SAMPLE_HZ) $error("invalid sample scheduler rate");
  $readmemh(SINE_FILE,sine);
 end
 // A variable boot seed cannot be implemented as a flip-flop asynchronous
 // set/reset value. Keep its load synchronous; all validity/control resets stay
 // asynchronous. The enclosing frontend holds reset for three proc edges.
 always @(posedge clk) begin
  if(rst)seq<=origin_raw;
  else if(!start&&enable&&tick)seq<=seq+1'b1;
 end
 always @(posedge clk or posedge rst) begin
  if(rst) begin
   phase<=0;pattern_position<=0;pending_gap<=0;
   tone0<=0;tone1<=0;tone2<=0;tone3<=0;noise<=32'h70210001;
   m_valid<=0;m_data<=0;m_capture_id<=0;m_raw_index<=0;m_flags<=0;raw_count<=0;drop_count<=0;
  end else if(start) begin
   phase<=0;pattern_position<=0;pending_gap<=0;m_valid<=0;
   tone0<=0;tone1<=0;tone2<=0;tone3<=0;noise<=32'h70210001;
  end else begin
   if(m_valid&&m_ready) m_valid<=0;
   if(enable) begin
    phase<=tick ? phase_sum-CLK_HZ : phase_sum;
    if(tick) begin
     raw_count<=raw_count+1'b1;pattern_position<=pattern_position+1'b1;
     tone0<=tone0+STEP0;tone1<=tone1+STEP1;tone2<=tone2+STEP2;tone3<=tone3+STEP3;
     noise<={noise[30:0],noise[31]^noise[21]^noise[1]^noise[0]};
     if(available) begin
      m_valid<=1;m_capture_id<=capture_id;m_raw_index<=seq;
      m_flags<=8'h01 | (pending_gap ? 8'h04 : 8'h00);pending_gap<=0;
      case(pattern_id)
       2'd1:m_data<=pattern_position[23:0];
       2'd2:m_data<=pattern_position[9:0]==0 ? 24'sh400000 : 24'sd0;
       2'd3:m_data<=mixed[23:0];
       default:m_data<=0;
      endcase
     end else begin drop_count<=drop_count+1'b1;pending_gap<=1;end
    end
   end
  end
 end
endmodule
