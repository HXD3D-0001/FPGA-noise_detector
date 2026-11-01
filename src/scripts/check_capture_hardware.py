"""Validate the separate CAP1/DAT1/CTR1 candidate; never authorize execution."""
import argparse
import hashlib
import importlib.util
import json
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from check_dma_hardware import parameters, one, check_dma, memory_plan

class Nets:
    def __init__(self,root):
        self.root=root
        ms=root.findall('./MODULES/MODULE');self.modules={m.get('INSTANCE'):m for m in ms}
        if len(ms)!=len(self.modules):raise ValueError('Duplicate module')
    def port(self,i,p):return one(self.modules[i],f"./PORTS/PORT[@NAME='{p}']")
    def bus(self,i,b):return one(self.modules[i],f"./BUSINTERFACES/BUSINTERFACE[@NAME='{b}']")
    @staticmethod
    def endpoints(p):
        es=p.findall('./CONNECTIONS/CONNECTION');out={(e.get('INSTANCE'),e.get('PORT')) for e in es}
        if len(es)!=len(out):raise ValueError('Duplicate connection')
        return out
    def net(self,a,b,exclusive=False):
        x,y=self.port(*a),self.port(*b)
        if not x.get('SIGNAME') or x.get('SIGNAME')!=y.get('SIGNAME') or a not in self.endpoints(y) or b not in self.endpoints(x):raise ValueError('Disconnected net '+str((a,b)))
        if exclusive and (self.endpoints(x)!={b} or self.endpoints(y)!={a}):raise ValueError('Extra net endpoint')
    def link(self,a,b,kind):
        x,y=self.bus(*a),self.bus(*b)
        if x.get('VLNV')!='xilinx.com:interface:'+kind+':1.0' or y.get('VLNV')!=x.get('VLNV') or not x.get('BUSNAME') or x.get('BUSNAME')!=y.get('BUSNAME'):raise ValueError('Bus identity mismatch')
        peers={(n,i.get('NAME')) for n,m in self.modules.items() for i in m.findall('./BUSINTERFACES/BUSINTERFACE') if i.get('BUSNAME')==x.get('BUSNAME')}
        if peers!={a,b}:raise ValueError('Bus endpoint mismatch')
        maps=[]
        for interface in (x,y):
            xs=interface.findall('./PORTMAPS/PORTMAP');pm={p.get('LOGICAL'):p.get('PHYSICAL') for p in xs}
            if not pm or len(pm)!=len(xs):raise ValueError('Bus signal map missing/duplicate')
            maps.append(pm)
        if set(maps[0])!=set(maps[1]):raise ValueError('Bus signal sets differ')
        for endpoint,interface in ((a,x),(b,y)):
            cfg=parameters(interface,'./PARAMETER')
            if endpoint[0] in ('hp0','hp1') and not cfg:continue
            if cfg.get('FREQ_HZ')!='50000000' or cfg.get('CLK_DOMAIN')!='capture_dma_ps7_0_FCLK_CLK0':raise ValueError('AXI clock metadata drift')
            if kind=='aximm':
                protocol='AXI4LITE' if a[0]=='control_connect' else ('AXI4' if a[0].startswith('dma_') else 'AXI3')
                mode='WRITE_ONLY' if a[1]=='M_AXI_S2MM' else 'READ_WRITE'
                strobe='0' if endpoint[0].startswith('dma_') and endpoint[1]=='S_AXI_LITE' else '1'
                if cfg.get('PROTOCOL')!=protocol or cfg.get('READ_WRITE_MODE')!=mode or cfg.get('HAS_WSTRB')!=strobe:raise ValueError('AXI protocol/strobe drift')
        if kind=='axis':required={'TVALID','TREADY','TDATA','TKEEP','TLAST'}
        else:
            required={'AWADDR','AWVALID','AWREADY','WDATA','WVALID','WREADY','BRESP','BVALID','BREADY'}
            if a[1]!='M_AXI_S2MM':required|={'ARADDR','ARVALID','ARREADY','RDATA','RRESP','RVALID','RREADY'}
            if b[1]!='S_AXI_LITE':required.add('WSTRB')
            if a[1] not in ('M02_AXI','M03_AXI','M04_AXI','M00_AXI','M01_AXI') or a[0]!='control_connect':
                required|={'AWLEN','AWSIZE','AWBURST','WLAST'}
                if a[1]!='M_AXI_S2MM':required|={'ARLEN','ARSIZE','ARBURST','RLAST'}
        if not required<=set(maps[0]):raise ValueError('Required bus signal missing')
        for sig in maps[0]:self.net((a[0],maps[0][sig]),(b[0],maps[1][sig]),True)
        if kind=='axis':
            for interface in (x,y):
                p=parameters(interface,'./PARAMETER')
                if any(p.get(k)!=v for k,v in {'TDATA_NUM_BYTES':'8','HAS_TREADY':'1','HAS_TKEEP':'1','HAS_TLAST':'1'}.items()):raise ValueError('Stream interface drift')

CANDIDATE_PS_EXTERNAL={k:'0' for k in ('PCW_EN_QSPI','PCW_EN_SDIO0','PCW_EN_SDIO1','PCW_EN_USB0','PCW_EN_USB1','PCW_EN_ENET0','PCW_EN_ENET1','PCW_EN_UART0','PCW_EN_UART1','PCW_EN_EMIO_TTC0')}

def topology(root,ps_external_expected=None):
    if ps_external_expected is None:ps_external_expected=CANDIDATE_PS_EXTERNAL
    n=Nets(root);ms=n.modules
    expected={'ps7':'processing_system7','processing':'capture_platform_top','control_connect':'smartconnect','dma_spectrum':'axi_dma','dma_pcm':'axi_dma','hp0':'axi_interconnect','hp1':'axi_interconnect','irqs':'xlconcat','locked':'xlconstant','reset':'proc_sys_reset'}
    if set(ms)!=set(expected) or any(ms[k].get('MODTYPE')!=v for k,v in expected.items()):raise ValueError('Wrong candidate module set')
    for k in ('dma_spectrum','dma_pcm'):check_dma(parameters(ms[k]))
    for hp in ('hp0','hp1'):
        if any(parameters(ms[hp]).get(k)!=v for k,v in {'NUM_SI':'2','NUM_MI':'1','STRATEGY':'1'}.items()):raise ValueError('HP topology drift')
    if any(parameters(ms['control_connect']).get(k)!=v for k,v in {'NUM_SI':'1','NUM_MI':'5'}.items()):raise ValueError('GP topology drift')
    ps=parameters(ms['ps7'])
    for k in ['PCW_EN_DDR','PCW_USE_M_AXI_GP0','PCW_USE_S_AXI_HP0','PCW_USE_S_AXI_HP1','PCW_EN_TTC0','PCW_USE_FABRIC_INTERRUPT']:
        if ps.get(k)!='1':raise ValueError('Required PS interface missing '+k)
    for k,v in ps_external_expected.items():
        if ps.get(k)!=v:raise ValueError('Unexpected PS external peripheral '+k)
    if abs(float(ps['PCW_FPGA0_PERIPHERAL_FREQMHZ'])-50)>1e-6:raise ValueError('Wrong FCLK')
    plan=memory_plan(int(ps['PCW_DDR_RAM_BASEADDR'],0),int(ps['PCW_DDR_RAM_HIGHADDR'],0))
    cfg=parameters(ms['processing'])
    for k,v in {'CLK_HZ':'50000000','FFT_CLOCK_GATE':'1','BOOT_ORIGIN':'0','ADC_WARMUP_CYCLES':'100000000','START_TIMEOUT_CYCLES':'250000000','CONTROL_TIMEOUT_CYCLES':'250000000','OWNER_TIMEOUT_CYCLES':'5000000'}.items():
        if cfg.get(k)!=v:raise ValueError('Processing parameter drift '+k)
    links=[(('ps7','M_AXI_GP0'),('control_connect','S00_AXI'),'aximm')]
    for idx,bank in zip((2,3,4),('cap','dat','ctr')):links.append((('control_connect',f'M0{idx}_AXI'),('processing',f's_{bank}_axi'),'aximm'))
    for idx,flow in enumerate(('spectrum','pcm')):
        dma='dma_'+flow;hp='hp'+str(idx)
        links += [(('control_connect',f'M0{idx}_AXI'),(dma,'S_AXI_LITE'),'aximm'),((dma,'M_AXI_S2MM'),(hp,'S00_AXI'),'aximm'),((dma,'M_AXI_SG'),(hp,'S01_AXI'),'aximm'),((hp,'M00_AXI'),('ps7',f'S_AXI_HP{idx}'),'aximm'),(('processing',f'm_{flow}_axis'),(dma,'S_AXIS_S2MM'),'axis')]
    for a,b,kind in links:n.link(a,b,kind)
    clocks=[('ps7',p) for p in ('M_AXI_GP0_ACLK','S_AXI_HP0_ACLK','S_AXI_HP1_ACLK')]+[('processing','clk'),('control_connect','aclk'),('reset','slowest_sync_clk')]
    clocks += [(hp,p) for hp in ('hp0','hp1') for p in ('ACLK','S00_ACLK','S01_ACLK','M00_ACLK')]
    clocks += [(dma,p) for dma in ('dma_spectrum','dma_pcm') for p in ('s_axi_lite_aclk','m_axi_sg_aclk','m_axi_s2mm_aclk')]
    for sink in clocks:
        n.net(('ps7','FCLK_CLK0'),sink)
        if sink[0] not in ('hp0','hp1') and n.port(*sink).get('CLKFREQUENCY')!='50000000':raise ValueError('Clock sink frequency')
    if n.port('ps7','FCLK_CLK0').get('CLKFREQUENCY')!='50000000':raise ValueError('Clock source frequency')
    resets=[('processing','aresetn'),('control_connect','aresetn')]+[(hp,p) for hp in ('hp0','hp1') for p in ('ARESETN','S00_ARESETN','S01_ARESETN','M00_ARESETN')]+[(d,'axi_resetn') for d in ('dma_spectrum','dma_pcm')]
    for sink in resets:
        n.net(('reset','peripheral_aresetn'),sink)
        if sink[0] not in ('hp0','hp1') and n.port(*sink).get('POLARITY')!='ACTIVE_LOW':raise ValueError('Reset sink polarity')
    n.net(('ps7','FCLK_RESET0_N'),('reset','ext_reset_in'))
    if parameters(ms['reset']).get('C_EXT_RESET_HIGH')!='0':raise ValueError('Reset polarity')
    n.net(('locked','dout'),('reset','dcm_locked'))
    if int(parameters(ms['locked']).get('CONST_VAL','-1'),0)!=1:raise ValueError('Reset lock source')
    registers={};ranges=ms['ps7'].findall('./MEMORYMAP/MEMRANGE')
    if len(ranges)!=5:raise ValueError('Require five independent GP0 regions')
    for m in ranges:
        inst,bus=m.get('INSTANCE'),m.get('SLAVEBUSINTERFACE');key=bus if inst=='processing' else inst
        size=4096 if inst=='processing' else 65536
        if (inst=='processing' and bus not in ('s_cap_axi','s_dat_axi','s_ctr_axi')) or (inst!='processing' and (inst not in ('dma_spectrum','dma_pcm') or bus!='S_AXI_LITE')) or key in registers or m.get('MASTERBUSINTERFACE')!='M_AXI_GP0' or m.get('MEMTYPE')!='REGISTER':raise ValueError('Register region identity')
        lo,hi=int(m.get('BASEVALUE','-1'),0),int(m.get('HIGHVALUE','-1'),0)
        if lo<0x40000000 or hi>0x7fffffff or lo%size or hi-lo+1!=size:raise ValueError('Register bounds')
        registers[key]={'base':lo,'high':hi,'bytes':size}
    order=sorted(registers.values(),key=lambda p:p['base'])
    if any(a['high']>=b['base'] for a,b in zip(order,order[1:])):raise ValueError('Overlapping MMIO')
    for b in ('s_cap_axi','s_dat_axi','s_ctr_axi'):
        blocks=[x for x in ms['processing'].findall('./ADDRESSBLOCKS/ADDRESSBLOCK') if x.get('INTERFACE')==b]
        if len(blocks)!=1 or int(blocks[0].get('RANGE','0'),0)!=4096:raise ValueError('Register aperture')
    for idx,dma in enumerate(('dma_spectrum','dma_pcm')):
        rs=ms[dma].findall('./MEMORYMAP/MEMRANGE')
        if len(rs)!=2 or {x.get('MASTERBUSINTERFACE') for x in rs}!={'M_AXI_SG','M_AXI_S2MM'}:raise ValueError('DMA memory mappings')
        for m in rs:
            if int(m.get('BASEVALUE','-1'),0)!=0 or int(m.get('HIGHVALUE','-1'),0)!=int(ps['PCW_DDR_RAM_HIGHADDR'],0) or m.get('MEMTYPE')!='MEMORY' or m.get('INSTANCE')!='ps7' or m.get('SLAVEBUSINTERFACE')!=f'S_AXI_HP{idx}':raise ValueError('DMA DDR bounds/path')
    if parameters(ms['irqs']).get('NUM_PORTS')!='2':raise ValueError('IRQ count')
    for idx,dma in enumerate(('dma_spectrum','dma_pcm')):
        n.net((dma,'s2mm_introut'),('irqs',f'In{idx}'),True)
        if n.port(dma,'s2mm_introut').get('SENSITIVITY')!='LEVEL_HIGH' or parameters(ms['irqs']).get(f'IN{idx}_WIDTH')!='1':raise ValueError('IRQ trigger/shape')
    n.net(('irqs','dout'),('ps7','IRQ_F2P'),True)
    if n.port('ps7','IRQ_F2P').get('SENSITIVITY')!='LEVEL_HIGH:LEVEL_HIGH':raise ValueError('IRQ PS trigger')
    if n.port('ps7','IRQ_F2P').get('LEFT')!='1' or n.port('ps7','IRQ_F2P').get('RIGHT')!='0':raise ValueError('IRQ width')
    externals=root.findall('./EXTERNALINTERFACES/BUSINTERFACE')
    if len(externals)!=2 or {b.get('NAME') for b in externals}!={'DDR_0','FIXED_IO_0'}:raise ValueError('External PS interfaces')
    physical=set()
    for bus in ('DDR','FIXED_IO'):
        inner=n.bus('ps7',bus);outer=one(root,f"./EXTERNALINTERFACES/BUSINTERFACE[@NAME='{bus}_0']")
        if inner.get('BUSNAME')!=outer.get('BUSNAME'):raise ValueError('External PS bus')
        im={x.get('LOGICAL'):x.get('PHYSICAL') for x in inner.findall('./PORTMAPS/PORTMAP')};om={x.get('LOGICAL'):x.get('PHYSICAL') for x in outer.findall('./PORTMAPS/PORTMAP')}
        if set(im)!=set(om):raise ValueError('External PS maps')
        for sig,p in om.items():
            physical.add(p);ep=one(root,f"./EXTERNALPORTS/PORT[@NAME='{p}']");ip=n.port('ps7',im[sig])
            if ep.get('SIGNAME')!=ip.get('SIGNAME') or n.endpoints(ep)!={('ps7',im[sig])}:raise ValueError('External PS pin')
    for p,direction in [('ref_clk_in','I'),('bclk_in','I'),('lrck_in','I'),('sdata_in','I'),('mclk_out','O')]:
        physical.add(p);ep=one(root,f"./EXTERNALPORTS/PORT[@NAME='{p}']");ip=n.port('processing',p)
        if ep.get('DIR')!=direction or ep.get('SIGNAME')!=ip.get('SIGNAME') or n.endpoints(ep)!={('processing',p)}:raise ValueError('Physical audio port disconnected')
    eps=root.findall('./EXTERNALPORTS/PORT')
    if len(eps)!=len(physical) or {x.get('NAME') for x in eps}!=physical:raise ValueError('Unexpected external ports')
    return {'registers':registers,'memory_plan':plan,'checked_axi_links':len(links),'clock_hz':50000000,'dma_irqs':{'dma_spectrum':{'gic_id':61,'sdt_encoded':0x401d},'dma_pcm':{'gic_id':62,'sdt_encoded':0x401e}},'physical_ports':['ref_clk_in','bclk_in','lrck_in','sdata_in','mclk_out']}

def validate(path,ps_external_expected=None,scope='CAP1_DAT1_CTR1_CANDIDATE_METADATA_ONLY',board_profile_status='UNVERIFIED_FITTED_DDR_REVISION_AND_AUDIO_IO'):
    with zipfile.ZipFile(path) as z:
        names=z.namelist()
        if len(names)!=len(set(names)) or any(n.lower().endswith(('.bit','.elf')) for n in names):raise ValueError('Invalid payload')
        if {n for n in names if n.endswith('.hwh')}!={'capture_dma.hwh','capture_dma_control_connect_0.hwh'}:raise ValueError('Unexpected HWH set')
        root=ET.fromstring(z.read('capture_dma.hwh'))
        if root.get('VIVADOVERSION')!='2026.1':raise ValueError('Wrong Vivado version')
        device=ET.fromstring(z.read('xsa.xml')).find('.//Device')
        if device is None or device.get('FpgaDevice')!='zynq:xc7z020:clg484:-2':raise ValueError('Wrong device')
        wiring=topology(root,ps_external_expected)
        spec=importlib.util.spec_from_file_location('init_review',Path(__file__).resolve().parents[1]/'tests/python/test_ps_hardware.py');review=importlib.util.module_from_spec(spec);spec.loader.exec_module(review)
        summaries={};risks=[]
        for name in ('ps7_init.c','ps7_init_gpl.c','ps7_init.tcl'):
            summary,found=review.inspect_init(name,z.read(name).decode('utf-8'))
            if summary['analysis_status']!='KNOWN':raise ValueError('Unknown generated init structure')
            summaries[name]=summary;risks+=found
    return {'status':'PASS','scope':scope,'xsa_sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest(),'board_execution_authorized':False,'init_execution_authorized':False,'board_profile_status':board_profile_status,'topology':wiring,'init_summary':summaries,'init_risks':risks}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('xsa',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    record={'status':'RUNNING','board_execution_authorized':False,'init_execution_authorized':False}
    a.output.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    try:record=validate(a.xsa)
    except Exception as e:
        record.update(status='FAIL',error=str(e));a.output.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');raise
    a.output.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');print('PASS capture_hardware CANDIDATE_METADATA_ONLY')
if __name__=='__main__':main()
