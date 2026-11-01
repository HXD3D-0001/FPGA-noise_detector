// Thin standard-named AXI interface for the control_status service.
module control_axi #(
 parameter CONTROL_TIMEOUT_TICKS=5000000, // 100 ms at 50 MHz
 parameter SNAPSHOT_TIMEOUT_TICKS=50000,
 parameter SNAPSHOT_STALE_TICKS=50000000
)(
(* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK", X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi, ASSOCIATED_RESET aresetn, FREQ_HZ 50000000" *)
 input wire clk,
(* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST", X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
 input wire aresetn,
 input wire [11:0] s_axi_awaddr,input wire s_axi_awvalid,output wire s_axi_awready,
 input wire [31:0] s_axi_wdata,input wire [3:0] s_axi_wstrb,input wire s_axi_wvalid,output wire s_axi_wready,
 output wire [1:0] s_axi_bresp,output wire s_axi_bvalid,input wire s_axi_bready,
 input wire [11:0] s_axi_araddr,input wire s_axi_arvalid,output wire s_axi_arready,
 output wire [31:0] s_axi_rdata,output wire [1:0] s_axi_rresp,output wire s_axi_rvalid,input wire s_axi_rready,
 input wire [31:0] state_i,status_i,irq_events_i,output wire irq,
 output wire command_req,output wire [2:0] command_kind,output wire [31:0] command_seq,
 input wire command_ack,command_error,input wire [31:0] command_ack_seq,
 output wire commit_req,output wire [31:0] commit_mode,commit_config_id,commit_seq,
 input wire commit_ack,commit_error,input wire [31:0] commit_ack_seq,
 output wire [31:0] mode_active,config_id_active,
 output wire snapshot_req,output wire [31:0] snapshot_request_seq,
 input wire snapshot_ack,snapshot_error,snapshot_invalidate,input wire [31:0] snapshot_ack_seq,
 // Counters: capture/raw/rx_drop/saturation/fft_drop/pcm_drop/protocol/clock,
 // adc_count/adc_clip/fir0/fir1/fir2/energy_drop/raw_drop/energy_highwater.
 input wire [1023:0] snapshot_counters_i,
 input wire[63:0]snapshot_energy_flush_i,
 input wire[63:0]snapshot_pcm_flush_i,
 input wire[255:0]snapshot_sequences_i,
 input wire [31:0] snapshot_source_id_i,input wire snapshot_position_valid_i,
 input wire [63:0] snapshot_latest_raw_i,snapshot_origin_raw_i,
 output wire recovery_req,input wire recovery_ack,
 input wire energy_valid,energy_invalidate,output wire energy_ready,input wire [31:0] energy_count_i,
 input wire [63:0] energy_seq,energy_capture_id,energy_first_raw,energy_value,
 input wire [31:0] energy_config_id,input wire [7:0] energy_flags,
 input wire[31:0]dma_reset_request_seq_i,input wire[1:0]dma_reset_request_mask_i,input wire dma_reset_request_reset_i,
 output wire dma_reset_ack_valid,output wire[31:0]dma_reset_ack_seq,output wire[1:0]dma_reset_ack_mask,
 output wire flow_req,output wire[1:0]flow_mask,output wire[31:0]flow_seq,
 input wire flow_ack,flow_error,input wire[31:0]flow_ack_seq
);
 control_status #(.CONTROL_TIMEOUT_TICKS(CONTROL_TIMEOUT_TICKS),.SNAPSHOT_TIMEOUT_TICKS(SNAPSHOT_TIMEOUT_TICKS),.SNAPSHOT_STALE_TICKS(SNAPSHOT_STALE_TICKS)) impl(
 .clk(clk),.aresetn(aresetn),.awaddr(s_axi_awaddr),.awvalid(s_axi_awvalid),.awready(s_axi_awready),.wdata(s_axi_wdata),.wstrb(s_axi_wstrb),.wvalid(s_axi_wvalid),.wready(s_axi_wready),.bresp(s_axi_bresp),.bvalid(s_axi_bvalid),.bready(s_axi_bready),.araddr(s_axi_araddr),.arvalid(s_axi_arvalid),.arready(s_axi_arready),.rdata(s_axi_rdata),.rresp(s_axi_rresp),.rvalid(s_axi_rvalid),.rready(s_axi_rready),.state_i(state_i),.status_i(status_i),.irq_events_i(irq_events_i),.irq(irq),.command_req(command_req),.command_kind(command_kind),.command_seq(command_seq),.command_ack(command_ack),.command_error(command_error),.command_ack_seq(command_ack_seq),.commit_req(commit_req),.commit_mode(commit_mode),.commit_config_id(commit_config_id),.commit_seq(commit_seq),.commit_ack(commit_ack),.commit_error(commit_error),.commit_ack_seq(commit_ack_seq),.mode_active(mode_active),.config_id_active(config_id_active),.snapshot_req(snapshot_req),.snapshot_request_seq(snapshot_request_seq),.snapshot_ack(snapshot_ack),.snapshot_error(snapshot_error),.snapshot_invalidate(snapshot_invalidate),.snapshot_ack_seq(snapshot_ack_seq),.snapshot_counters_i(snapshot_counters_i),.snapshot_energy_flush_i(snapshot_energy_flush_i),.snapshot_pcm_flush_i(snapshot_pcm_flush_i),.snapshot_sequences_i(snapshot_sequences_i),.snapshot_source_id_i(snapshot_source_id_i),.snapshot_position_valid_i(snapshot_position_valid_i),.snapshot_latest_raw_i(snapshot_latest_raw_i),.snapshot_origin_raw_i(snapshot_origin_raw_i),.recovery_req(recovery_req),.recovery_ack(recovery_ack),.energy_valid(energy_valid),.energy_invalidate(energy_invalidate),.energy_ready(energy_ready),.energy_count_i(energy_count_i),.energy_seq(energy_seq),.energy_capture_id(energy_capture_id),.energy_first_raw(energy_first_raw),.energy_value(energy_value),.energy_config_id(energy_config_id),.energy_flags(energy_flags),.dma_reset_request_seq_i(dma_reset_request_seq_i),.dma_reset_request_mask_i(dma_reset_request_mask_i),.dma_reset_request_reset_i(dma_reset_request_reset_i),.dma_reset_ack_valid(dma_reset_ack_valid),.dma_reset_ack_seq(dma_reset_ack_seq),.dma_reset_ack_mask(dma_reset_ack_mask),.flow_req(flow_req),.flow_mask(flow_mask),.flow_seq(flow_seq),.flow_ack(flow_ack),.flow_error(flow_error),.flow_ack_seq(flow_ack_seq));
endmodule

