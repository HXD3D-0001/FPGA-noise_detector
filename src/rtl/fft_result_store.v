module fft_result_store(
 input wire clk,rst,reserve_valid, output wire reserve_ready,
 input wire [63:0] reserve_capture_id,reserve_first_raw,reserve_frame_seq,
 input wire [31:0] reserve_config_id,input wire [7:0] reserve_flags,
 input wire abort,commit,result_valid,output wire result_ready,
 input wire signed [23:0] result_real,result_imag,
 input wire [11:0] result_index,input wire [7:0] result_blk_exp,input wire result_last,
 output wire collect_done,output reg collect_error,output wire [7:0] collect_blk_exp,
 output wire packet_valid,input wire packet_ready,
 output wire [15:0] packet_type,packet_flags,
 output wire [63:0] packet_capture_id,packet_seq,packet_first_raw,
 output wire [31:0] packet_config_id,output wire [7:0] packet_blk_exp,
 input wire read_en,input wire [12:0] read_addr,
 output reg read_valid,output wire [31:0] read_data,input wire packet_done,
 output reg [63:0] completed_count,drop_count,invalid_count
);
 localparam [2:0] FREE=0,FILL=1,COMPLETE=2,READY=3,READING=4;
 // 48 physical bits recover the protocol's two signed32 words exactly.
 (* ram_style="block" *) reg [47:0] ram0[0:4095],ram1[0:4095];
 reg [47:0] read0,read1;reg read_bank,read_imag;
 reg [2:0] state0,state1;
 reg [63:0] cap0,cap1,seq0,seq1,raw0,raw1;
 reg [31:0] cfg0,cfg1;reg [7:0] flags0,flags1,exp0,exp1;
 reg private_active,private_bank,reading,owner;
 reg [11:0] next_index;reg [7:0] active_exp;
 reg gap_pending,contract_valid,poison,sequence_valid;
 reg [63:0] highest_capture,last_sequence;reg [31:0] bound_config;
 wire newer=!contract_valid||reserve_capture_id>highest_capture;
 wire structural=reserve_capture_id!=0&&reserve_config_id!=0&&reserve_flags[0]&&
                 !reserve_flags[1]&&reserve_flags[7:6]==0&&
                 reserve_first_raw<=64'hffffffffffffffff-64'd196608;
 wire identity_ok=newer||(reserve_capture_id==highest_capture&&
                         reserve_config_id==bound_config&&!poison);
 wire sequence_ok=!sequence_valid||reserve_frame_seq>last_sequence;
 wire descriptor_ok=structural&&identity_ok&&sequence_ok;
 wire releasing=reading&&packet_done;
 wire free0=state0==FREE||(releasing&&!owner);
 wire free1=state1==FREE||(releasing&&owner);
 assign reserve_ready=!rst&&!abort&&!private_active&&(free0||free1||!descriptor_ok);
 wire reserved=reserve_valid&&reserve_ready;
 wire selected=(state0!=READY)||((state1==READY)&&(seq1<seq0));
 assign packet_valid=!rst&&!reading&&(state0==READY||state1==READY);
 assign packet_type=16'd1;
 assign packet_flags={8'd0,selected?flags1:flags0};
 assign packet_capture_id=selected?cap1:cap0;
 assign packet_seq=selected?seq1:seq0;
 assign packet_first_raw=selected?raw1:raw0;
 assign packet_config_id=selected?cfg1:cfg0;
 assign packet_blk_exp=selected?exp1:exp0;
 wire [2:0] private_state=private_bank?state1:state0;
 assign collect_done=!rst&&private_active&&private_state==COMPLETE;
 assign collect_blk_exp=active_exp;
 assign result_ready=!rst&&!abort&&!commit&&private_active&&private_state==FILL;
 wire point_ok=result_index==next_index&&result_last==(next_index==12'd4095)&&
               result_blk_exp<=8'd12&&(next_index==0||result_blk_exp==active_exp);
 wire accepted_point=result_valid&&result_ready;
 wire write_enable=accepted_point&&point_ok;
 wire requested_read=!rst&&reading&&!packet_done&&read_en;
 wire [47:0] read_complex=read_bank?read1:read0;
 wire [23:0] read_component=read_imag?read_complex[47:24]:read_complex[23:0];
 assign read_data={{8{read_component[23]}},read_component};
 // RAM ports are synchronous and reset-free; ownership/control reset separately.
 always @(posedge clk)begin
  if(write_enable)begin
   if(private_bank)ram1[next_index]<={result_imag,result_real};
   else ram0[next_index]<={result_imag,result_real};
  end
  if(requested_read)begin
   if(owner)read1<=ram1[read_addr[12:1]];
   else read0<=ram0[read_addr[12:1]];
   read_bank<=owner;read_imag<=read_addr[0];
  end
  read_valid<=requested_read;
 end
 always @(posedge clk)begin
  if(rst)begin
   state0<=FREE;state1<=FREE;cap0<=0;cap1<=0;seq0<=0;seq1<=0;raw0<=0;raw1<=0;
   cfg0<=0;cfg1<=0;flags0<=0;flags1<=0;exp0<=0;exp1<=0;
   private_active<=0;private_bank<=0;reading<=0;owner<=0;next_index<=0;active_exp<=0;
   gap_pending<=0;contract_valid<=0;poison<=0;sequence_valid<=0;
   highest_capture<=0;last_sequence<=0;bound_config<=0;
   collect_error<=0;completed_count<=0;drop_count<=0;invalid_count<=0;
  end else begin
   collect_error<=0;
   if(releasing)begin
    if(owner)state1<=FREE;else state0<=FREE;
    reading<=0;
   end
   if(packet_valid&&packet_ready)begin
    owner<=selected;reading<=1;
    if(selected)state1<=READING;else state0<=READING;
   end
   if(abort)begin
    gap_pending<=1;
    if(private_active)begin
     if(private_bank)state1<=FREE;else state0<=FREE;
     private_active<=0;drop_count<=drop_count+1;
    end
   end else if(commit&&private_active)begin
    private_active<=0;
    if(private_state==COMPLETE)begin
     if(private_bank)begin state1<=READY;exp1<=active_exp;flags1<=flags1|(gap_pending?8'd4:8'd0);end
     else begin state0<=READY;exp0<=active_exp;flags0<=flags0|(gap_pending?8'd4:8'd0);end
     completed_count<=completed_count+1;gap_pending<=0;
    end else begin
     if(private_bank)state1<=FREE;else state0<=FREE;
     drop_count<=drop_count+1;invalid_count<=invalid_count+1;collect_error<=1;gap_pending<=1;
    end
   end else if(accepted_point)begin
    if(!point_ok)begin
     if(private_bank)state1<=FREE;else state0<=FREE;
     private_active<=0;drop_count<=drop_count+1;invalid_count<=invalid_count+1;
     collect_error<=1;gap_pending<=1;
    end else begin
     if(next_index==0)active_exp<=result_blk_exp;
     if(next_index==12'd4095)begin
      if(private_bank)state1<=COMPLETE;else state0<=COMPLETE;
     end else next_index<=next_index+1'b1;
    end
   end
   if(reserved)begin
    // Stale/malformed records never lower the capture or sequence barriers.
    if(newer&&structural)begin
     highest_capture<=reserve_capture_id;bound_config<=reserve_config_id;
     contract_valid<=1;poison<=0;
    end else if(contract_valid&&reserve_capture_id==highest_capture&&reserve_config_id!=bound_config)
     poison<=1;
    if(!descriptor_ok)begin
     invalid_count<=invalid_count+1;collect_error<=1;gap_pending<=1;
    end else begin
     last_sequence<=reserve_frame_seq;sequence_valid<=1;
     private_active<=1;private_bank<=!free0;next_index<=0;active_exp<=0;
     if(free0)begin
      state0<=FILL;cap0<=reserve_capture_id;seq0<=reserve_frame_seq;raw0<=reserve_first_raw;
      cfg0<=reserve_config_id;flags0<=reserve_flags|(gap_pending?8'd4:8'd0);
     end else begin
      state1<=FILL;cap1<=reserve_capture_id;seq1<=reserve_frame_seq;raw1<=reserve_first_raw;
      cfg1<=reserve_config_id;flags1<=reserve_flags|(gap_pending?8'd4:8'd0);
     end
    end
   end
  end
 end
endmodule
