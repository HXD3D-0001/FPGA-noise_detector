"""Validate actual offline two-SG-DMA XSA; never authorize board execution."""
import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import xml.etree.ElementTree as ET
import zipfile

EXPECTED={'C_INCLUDE_SG':'1','C_INCLUDE_MM2S':'0','C_INCLUDE_S2MM':'1',
          'C_SG_LENGTH_WIDTH':'23','C_M_AXI_S2MM_DATA_WIDTH':'64',
          'C_S_AXIS_S2MM_TDATA_WIDTH':'64','C_INCLUDE_S2MM_DRE':'0',
          'C_SG_INCLUDE_STSCNTRL_STRM':'0'}

def check_dma(fields):
    for key,value in EXPECTED.items():
        if fields.get(key)!=value:raise ValueError('DMA configuration drift: '+key)

def memory_plan(low,high):
    if low<0x100000 or low%64 or high<=low:raise ValueError('Invalid DDR range')
    # Seven MiB or more for APP+stacks/heap below the aligned dedicated data area.
    base=(low+7*1024*1024+0xfffff)&~0xfffff
    end=base+32*1024*1024
    if end-1>high:raise ValueError('DDR too small for dedicated 32MiB data budget')
    return {'app_base':low,'app_end':base,'data_base':base,'data_end':end,'data_bytes':end-base}

def one(parent, xpath):
    matches=parent.findall(xpath)
    if len(matches)!=1:raise ValueError('Require unique HWH element: '+xpath)
    return matches[0]

def parameters(element, xpath='./PARAMETERS/PARAMETER'):
    items=element.findall(xpath)
    result={p.get('NAME'):p.get('VALUE') for p in items}
    if len(items)!=len(result):raise ValueError('Duplicate HWH parameter')
    return result

def topology(root, ddr_low, ddr_high, plan):
    """Check both exported nets and address maps, not only IP settings."""
    modules={m.get('INSTANCE'):m for m in root.findall('./MODULES/MODULE')}
    if len(modules)!=len(root.findall('./MODULES/MODULE')):raise ValueError('Duplicate module')
    hp_kind=modules.get('hp0',ET.Element('missing')).get('MODTYPE')
    if hp_kind not in ('smartconnect','axi_interconnect'):
        raise ValueError('Unsupported HP interconnect')
    required={'control':'control_axi','control_connect':'smartconnect','hp0':hp_kind,
              'hp1':hp_kind,'reset':'proc_sys_reset','irqs':'xlconcat','ps7':'processing_system7',
              'dma_pcm':'axi_dma','dma_spectrum':'axi_dma','locked':'xlconstant'}
    closed='processing' in modules
    profile='CLOSED_TEST_PATTERN_CANDIDATE' if closed else 'BARE_DMA_CANDIDATE'
    if closed:required['processing']='monitor_service_top'
    if set(modules)!=set(required):raise ValueError('Unexpected candidate modules')
    for name,kind in required.items():
        if modules[name].get('MODTYPE')!=kind:raise ValueError('Wrong module kind: '+name)
    if hp_kind=='axi_interconnect':
        for name in ('hp0','hp1'):
            cfg=parameters(modules[name])
            for key,value in {'NUM_SI':'2','NUM_MI':'1','STRATEGY':'1'}.items():
                if cfg.get(key)!=value:raise ValueError('Area interconnect configuration drift: '+name+'/'+key)
    def port(instance,name):return one(modules[instance],f"./PORTS/PORT[@NAME='{name}']")
    def endpoints(p):
        xs=p.findall('./CONNECTIONS/CONNECTION')
        result={(c.get('INSTANCE'),c.get('PORT')) for c in xs}
        if len(xs)!=len(result):raise ValueError('Duplicate net connection')
        return result
    def net(source, destination):
        sp=port(*source);dp=port(*destination)
        if not sp.get('SIGNAME') or sp.get('SIGNAME')!=dp.get('SIGNAME'):
            raise ValueError('Net mismatch: '+str(destination))
        if source not in endpoints(dp) or destination not in endpoints(sp):
            raise ValueError('Asymmetric net connection: '+str(destination))
    def bus(instance,name):return one(modules[instance],f"./BUSINTERFACES/BUSINTERFACE[@NAME='{name}']")
    def required_signals(a,b,ab,bb):
        # These are fixed candidate paths, so metadata cannot weaken the
        # protocol contract by changing both endpoints to a smaller interface.
        stream=a[0]=='processing'
        kind='xilinx.com:interface:'+('axis' if stream else 'aximm')+':1.0'
        if ab.get('VLNV')!=kind or bb.get('VLNV')!=kind:raise ValueError('AXI interface kind drift')
        configs=[parameters(interface,'./PARAMETER') for interface in (ab,bb)]
        if stream:
            for cfg in configs:
                if any(cfg.get(key)!='1' for key in ('HAS_TREADY','HAS_TLAST','HAS_TKEEP')) or cfg.get('TDATA_NUM_BYTES')!='8':
                    raise ValueError('AXIS64 interface configuration drift')
            return {'TVALID','TREADY','TDATA','TLAST','TKEEP'}
        if a[0]=='control_connect':protocol,mode='AXI4LITE','READ_WRITE'
        elif a[0] in ('dma_spectrum','dma_pcm'):
            protocol='AXI4';mode='WRITE_ONLY' if a[1]=='M_AXI_S2MM' else 'READ_WRITE'
        else:protocol,mode='AXI3','READ_WRITE'
        for endpoint,cfg in zip((a,b),configs):
            # Only the hierarchical area wrapper lacks interface parameters;
            # its peer still supplies an actual checked protocol declaration.
            if hp_kind=='axi_interconnect' and endpoint[0] in ('hp0','hp1') and not cfg:continue
            if cfg.get('PROTOCOL')!=protocol or cfg.get('READ_WRITE_MODE')!=mode:
                raise ValueError('AXI protocol/read-write mode drift: '+str(endpoint))
            # AMD DMA's register slave is a full-word AXI Lite interface.
            # Its real HAS_WSTRB=0 declaration is the sole strobe exception;
            # SmartConnect advertises HAS_WSTRB=1 but exports no connected map.
            dma_lite=endpoint[0] in ('dma_spectrum','dma_pcm') and endpoint[1]=='S_AXI_LITE'
            if cfg.get('HAS_WSTRB')!=('0' if dma_lite else '1'):
                raise ValueError('AXI byte-strobe configuration drift: '+str(endpoint))
        required=set()
        if mode!='WRITE_ONLY':
            required.update(('ARADDR','ARVALID','ARREADY','RDATA','RRESP','RVALID','RREADY'))
            if protocol!='AXI4LITE':required.update(('ARLEN','ARSIZE','ARBURST','RLAST'))
        if mode!='READ_ONLY':
            required.update(('AWADDR','AWVALID','AWREADY','WDATA','WVALID','WREADY','BRESP','BVALID','BREADY'))
            if not (b[0] in ('dma_spectrum','dma_pcm') and b[1]=='S_AXI_LITE'):required.add('WSTRB')
            if protocol!='AXI4LITE':required.update(('AWLEN','AWSIZE','AWBURST','WLAST'))
        return required
    def bus_link(a,b):
        ab=bus(*a);bb=bus(*b)
        if not ab.get('BUSNAME') or ab.get('BUSNAME')!=bb.get('BUSNAME'):raise ValueError('AXI path mismatch: '+str(a))
        peers=[(n,x.get('NAME')) for n,m in modules.items() for x in m.findall('./BUSINTERFACES/BUSINTERFACE') if x.get('BUSNAME')==ab.get('BUSNAME')]
        if set(peers)!={a,b} or len(peers)!=2:raise ValueError('AXI bus has unexpected endpoints')
        # Corrupting only physical signal connections must not leave a valid bus-name proof.
        maps=[]
        for interface in (ab,bb):
            xs=interface.findall('./PORTMAPS/PORTMAP')
            pm={p.get('LOGICAL'):p.get('PHYSICAL') for p in xs}
            if not pm or len(pm)!=len(xs):raise ValueError('Missing or duplicate AXI port map')
            maps.append(pm)
        if set(maps[0])!=set(maps[1]):raise ValueError('AXI signal set mismatch')
        missing=required_signals(a,b,ab,bb)-set(maps[0])
        if missing:raise ValueError('Missing required AXI signals: '+str(a)+' '+','.join(sorted(missing)))
        for signal in maps[0]:
            ap=(a[0],maps[0][signal]);bp=(b[0],maps[1][signal]);net(ap,bp)
            if endpoints(port(*ap))!={bp} or endpoints(port(*bp))!={ap}:raise ValueError('AXI physical net has unexpected endpoints')
    paths=[(('ps7','M_AXI_GP0'),('control_connect','S00_AXI')),
           (('control_connect','M02_AXI'),('control','s_axi'))]
    for index,name in enumerate(('spectrum','pcm')):
        dma='dma_'+name;hp='hp'+str(index)
        paths.extend([(('control_connect',f'M0{index}_AXI'),(dma,'S_AXI_LITE')),
                      ((dma,'M_AXI_S2MM'),(hp,'S00_AXI')),((dma,'M_AXI_SG'),(hp,'S01_AXI')),
                      ((hp,'M00_AXI'),('ps7',f'S_AXI_HP{index}'))])
        if closed:paths.append((('processing',f'm_{name}_axis'),(dma,'S_AXIS_S2MM')))
    for a,b in paths:bus_link(a,b)
    for name,m in modules.items():
        for interface in m.findall('./BUSINTERFACES/BUSINTERFACE'):
            if interface.get('VLNV') in ('xilinx.com:interface:aximm:1.0','xilinx.com:interface:axis:1.0'):
                cfg=parameters(interface,'./PARAMETER')
                # Vivado's hierarchical AXI Interconnect HWH omits interface
                # clock parameters. Its full signal maps and every clock/reset
                # pin are checked below against the same 50 MHz source net.
                if hp_kind=='axi_interconnect' and name in ('hp0','hp1') and not cfg:
                    widths={'S00_AXI':'64','S01_AXI':'32','M00_AXI':'64'}
                    if interface.get('DATAWIDTH')!=widths.get(interface.get('NAME')):
                        raise ValueError('Area interconnect data width drift: '+name)
                    continue
                if cfg.get('FREQ_HZ')!='50000000' or cfg.get('CLK_DOMAIN')!='monitor_dma_ps7_0_FCLK_CLK0':
                    raise ValueError('AXI clock metadata drift: '+name)
    clock_sinks=[('ps7','M_AXI_GP0_ACLK'),('ps7','S_AXI_HP0_ACLK'),('ps7','S_AXI_HP1_ACLK'),
                 ('control','clk'),('reset','slowest_sync_clk'),('control_connect','aclk')]
    hp_clocks=('ACLK','S00_ACLK','S01_ACLK','M00_ACLK') if hp_kind=='axi_interconnect' else ('aclk',)
    clock_sinks += [(n,p) for n in ('hp0','hp1') for p in hp_clocks]
    clock_sinks += [(n,p) for n in ('dma_spectrum','dma_pcm') for p in ('s_axi_lite_aclk','m_axi_sg_aclk','m_axi_s2mm_aclk')]
    if closed:clock_sinks.append(('processing','clk'))
    for sink in clock_sinks:
        net(('ps7','FCLK_CLK0'),sink)
        frequency=port(*sink).get('CLKFREQUENCY')
        opaque_hp=hp_kind=='axi_interconnect' and sink[0] in ('hp0','hp1')
        if frequency!='50000000' and not (opaque_hp and frequency is None):raise ValueError('Non-50MHz fabric clock')
        if opaque_hp and port(*sink).get('SIGIS')!='clk':raise ValueError('Area interconnect clock pin type')
    if port('ps7','FCLK_CLK0').get('CLKFREQUENCY')!='50000000':raise ValueError('FCLK source frequency')
    hp_resets=('ARESETN','S00_ARESETN','S01_ARESETN','M00_ARESETN') if hp_kind=='axi_interconnect' else ('aresetn',)
    reset_sinks=[('control','aresetn'),('control_connect','aresetn')]+[(n,p) for n in ('hp0','hp1') for p in hp_resets]+[(n,'axi_resetn') for n in ('dma_spectrum','dma_pcm')]
    if closed:reset_sinks.append(('processing','aresetn'))
    for sink in reset_sinks:
        net(('reset','peripheral_aresetn'),sink)
        polarity=port(*sink).get('POLARITY')
        opaque_hp=hp_kind=='axi_interconnect' and sink[0] in ('hp0','hp1')
        if polarity!='ACTIVE_LOW' and not (opaque_hp and polarity is None):raise ValueError('Reset polarity mismatch')
        if opaque_hp and port(*sink).get('SIGIS')!='rst':raise ValueError('Area interconnect reset pin type')
    net(('ps7','FCLK_RESET0_N'),('reset','ext_reset_in'))
    for endpoint in [('ps7','FCLK_RESET0_N'),('reset','ext_reset_in'),('reset','peripheral_aresetn')]:
        if port(*endpoint).get('POLARITY')!='ACTIVE_LOW':raise ValueError('Reset source polarity')
    if parameters(modules['reset']).get('C_EXT_RESET_HIGH')!='0':raise ValueError('Reset IP configuration drift')
    net(('locked','dout'),('reset','dcm_locked'))
    if int(parameters(modules['locked']).get('CONST_VAL','-1'),0)!=1 or parameters(modules['locked']).get('CONST_WIDTH')!='1':raise ValueError('Reset lock source')
    if closed:
        cfg=parameters(modules['processing'])
        for key,value in {'BOOT_ORIGIN':'0','CLK_HZ':'50000000','FFT_CLOCK_GATE':'1','SERVICE_TIMEOUT_TICKS':'4999990'}.items():
            if cfg.get(key)!=value:raise ValueError('Processing profile drift: '+key)
        # The runtime uses this precise service ABI. Earlier closed archives
        # without FLOW or snapshot baselines cannot validate a current runtime.
        required_service={'flow_req':1,'flow_mask':2,'flow_seq':32,'flow_ack':1,
                          'flow_error':1,'flow_ack_seq':32,'snapshot_pcm_flush_i':64,
                          'snapshot_sequences_i':256}
        for name,width in required_service.items():
            for instance in ('processing','control'):
                p=port(instance,name)
                actual=abs(int(p.get('LEFT','0'))-int(p.get('RIGHT','0')))+1
                if actual!=width:raise ValueError('Current service ABI width: '+name)
        interfaces=root.findall('./EXTERNALINTERFACES/BUSINTERFACE')
        if {b.get('NAME') for b in interfaces}!={'DDR_0','FIXED_IO_0'} or len(interfaces)!=2:raise ValueError('Closed profile external interfaces')
        physical=set()
        for name in ('DDR','FIXED_IO'):
            internal=bus('ps7',name);external=one(root,f"./EXTERNALINTERFACES/BUSINTERFACE[@NAME='{name}_0']")
            if internal.get('BUSNAME')!=external.get('BUSNAME'):raise ValueError('External PS bus mismatch')
            ips={p.get('LOGICAL'):p.get('PHYSICAL') for p in internal.findall('./PORTMAPS/PORTMAP')}
            eps={p.get('LOGICAL'):p.get('PHYSICAL') for p in external.findall('./PORTMAPS/PORTMAP')}
            if set(ips)!=set(eps) or len(eps)!=len(external.findall('./PORTMAPS/PORTMAP')):raise ValueError('External PS port maps')
            for signal,pname in eps.items():
                physical.add(pname);p=one(root,f"./EXTERNALPORTS/PORT[@NAME='{pname}']");source=port('ps7',ips[signal])
                if p.get('SIGNAME')!=source.get('SIGNAME') or endpoints(p)!={('ps7',ips[signal])} or endpoints(source)!={('monitor_dma_imp',pname)}:raise ValueError('Closed profile external PS connection')
                if any(p.get(k)!=source.get(k) for k in ('DIR','LEFT','RIGHT')):raise ValueError('External PS pin shape')
        ports=root.findall('./EXTERNALPORTS/PORT')
        if {p.get('NAME') for p in ports}!=physical or len(ports)!=len(physical):raise ValueError('Closed profile exposes extra pins')
        service_ports=[p for p in modules['control'].findall('./PORTS/PORT') if p.get('NAME') not in {'clk','aresetn','irq','mode_active','config_id_active'} and not p.get('NAME').startswith('s_axi_')]
        expected_processing={'clk','aresetn'}|{p.get('NAME') for p in service_ports}|{f'm_{flow}_axis_{sig}' for flow in ('spectrum','pcm') for sig in ('tdata','tkeep','tvalid','tready','tlast')}
        if {p.get('NAME') for p in modules['processing'].findall('./PORTS/PORT')}!=expected_processing:raise ValueError('Processing service pin set mismatch')
        for p in service_ports:
            name=p.get('NAME');q=port('processing',name);net(('control',name),('processing',name))
            if {p.get('DIR'),q.get('DIR')}!={'I','O'} or (p.get('LEFT'),p.get('RIGHT'))!=(q.get('LEFT'),q.get('RIGHT')):raise ValueError('Service pin shape mismatch: '+name)
            if endpoints(p)!={('processing',name)} or endpoints(q)!={('control',name)}:raise ValueError('Service pin has unexpected endpoints: '+name)
    for name,source,kind in ([] if closed else [('proc_clk',('ps7','FCLK_CLK0'),'clk'),('proc_aresetn',('reset','peripheral_aresetn'),'rst')]):
        p=one(root,f"./EXTERNALPORTS/PORT[@NAME='{name}']")
        if p.get('SIGNAME')!=port(*source).get('SIGNAME') or p.get('SIGIS')!=kind or p.get('DIR')!='O':raise ValueError('External clock/reset mismatch')
        if ('External_Ports',name) not in endpoints(port(*source)):raise ValueError('External clock/reset disconnected')
        if kind=='clk' and p.get('CLKFREQUENCY')!='50000000':raise ValueError('External frequency')
        if kind=='rst' and p.get('POLARITY')!='ACTIVE_LOW':raise ValueError('External reset polarity')
    registers={};ranges=modules['ps7'].findall('./MEMORYMAP/MEMRANGE')
    if len(ranges)!=3:raise ValueError('Unexpected GP0 register regions')
    for m in ranges:
        name=m.get('INSTANCE');low=int(m.get('BASEVALUE','-1'),0);high=int(m.get('HIGHVALUE','-1'),0)
        expected_interface='s_axi' if name=='control' else 'S_AXI_LITE'
        size=4096 if name=='control' else 65536
        if name not in ('control','dma_pcm','dma_spectrum') or name in registers or m.get('MASTERBUSINTERFACE')!='M_AXI_GP0' or m.get('SLAVEBUSINTERFACE')!=expected_interface or m.get('MEMTYPE')!='REGISTER':raise ValueError('Wrong GP0 region identity')
        if low<0x40000000 or high>0x7fffffff or low%size or high-low+1!=size:raise ValueError('Wrong GP0 region bounds')
        registers[name]={'base':low,'high':high,'bytes':size}
    ordered=sorted(registers.values(),key=lambda r:r['base'])
    if any(a['high']>=b['base'] for a,b in zip(ordered,ordered[1:])):raise ValueError('Overlapping register regions')
    block=one(modules['control'],'./ADDRESSBLOCKS/ADDRESSBLOCK')
    if block.get('INTERFACE')!='s_axi' or int(block.get('RANGE','0'),0)!=4096:raise ValueError('Control must expose 4KiB')
    dma_ranges={}
    for index,name in enumerate(('dma_spectrum','dma_pcm')):
        ranges=modules[name].findall('./MEMORYMAP/MEMRANGE')
        if len(ranges)!=2 or {m.get('MASTERBUSINTERFACE') for m in ranges}!={'M_AXI_SG','M_AXI_S2MM'}:raise ValueError('Missing DMA SG/data memory range')
        dma_ranges[name]={}
        for m in ranges:
            low=int(m.get('BASEVALUE','-1'),0);high=int(m.get('HIGHVALUE','-1'),0)
            if m.get('INSTANCE')!='ps7' or m.get('SLAVEBUSINTERFACE')!=f'S_AXI_HP{index}' or m.get('MEMTYPE')!='MEMORY' or low!=0 or high!=ddr_high or low>ddr_low or high<plan['data_end']-1:raise ValueError('Wrong DMA HP DDR/OCM range')
            dma_ranges[name][m.get('MASTERBUSINTERFACE')]={'base':low,'high':high,'hp':index}
    irqs=parameters(modules['irqs'])
    if irqs.get('NUM_PORTS')!='3' or irqs.get('dout_width')!='3':raise ValueError('IRQ concat width')
    interrupts={}
    for index,(name,pname) in enumerate([('dma_spectrum','s2mm_introut'),('dma_pcm','s2mm_introut'),('control','irq')]):
        net((name,pname),('irqs',f'In{index}'))
        if endpoints(port(name,pname))!={('irqs',f'In{index}')} or endpoints(port('irqs',f'In{index}'))!={(name,pname)}:raise ValueError('IRQ endpoint mapping')
        inp=port('irqs',f'In{index}')
        if inp.get('LEFT')!='0' or inp.get('RIGHT')!='0' or irqs.get(f'IN{index}_WIDTH')!='1' or port(name,pname).get('SENSITIVITY')!='LEVEL_HIGH':raise ValueError('IRQ input shape/trigger')
        # Zynq IRQ_F2P[0..2] are GIC SPI 61..63. SDT stores SPI number
        # relative to 32 and trigger 4 in bits 15:12 (xinterrupt_wrap.h).
        interrupts[name]={'concat_bit':index,'gic_id':61+index,'sdt_spi':29+index,'trigger':4,'sdt_encoded':0x4000|(29+index)}
    net(('irqs','dout'),('ps7','IRQ_F2P'))
    for endpoint in [('irqs','dout'),('ps7','IRQ_F2P')]:
        if port(*endpoint).get('LEFT')!='2' or port(*endpoint).get('RIGHT')!='0':raise ValueError('IRQ output width')
    if port('ps7','IRQ_F2P').get('SENSITIVITY')!='LEVEL_HIGH:LEVEL_HIGH:LEVEL_HIGH':raise ValueError('IRQ PS trigger')
    return {'profile':profile,'service_abi':'FLOW_SINGLE_ENGINE_AND_FOUR_SEQUENCE_BASELINES' if closed else None,
            'hp_interconnect':'axi_interconnect_area' if hp_kind=='axi_interconnect' else 'smartconnect_legacy',
            'registers':registers,'dma_ranges':dma_ranges,'interrupts':interrupts,
            'clock_hz':50000000,'reset_polarity':'ACTIVE_LOW','checked_axi_links':len(paths),
            'interrupt_encoding_basis':'Zynq IRQ_F2P[0..2] -> GIC 61..63; SDT SPI offset32, trigger4 shift12'}

def check_bsp_interrupts(include, wiring):
    """Optional generated-header crosscheck. No hardware IRQ behavior is inferred."""
    header=Path(include)/'xparameters.h';wrapper=Path(include)/'xinterrupt_wrap.h'
    text=header.read_text(encoding='utf-8');wrap=wrapper.read_text(encoding='utf-8')
    def define(source,name):
        xs=re.findall(r'^\s*#define\s+'+re.escape(name)+r'\s+([^\r\n]+)',source,re.M)
        if len(xs)!=1:raise ValueError('Missing or duplicate BSP define: '+name)
        return xs[0].split('/*')[0].strip()
    for name,expected in {'XINTC_INTRID_MASK':0xfff,'XINTC_TRIGGER_MASK':0xf000,
                          'XINTC_TRIGGER_SHIFT':12,'XINTC_INTR_TYPE_MASK':0x100000,
                          'XINTC_INTR_TYPE_SHIFT':20,'XIL_TRIG_ACTIVE_HIGH_LEVEL':4}.items():
        if int(define(wrap,name).rstrip('UuLl'),0)!=expected:raise ValueError('BSP interrupt encoding drift: '+name)
    offset=re.sub(r'\s+','',wrap)
    expected='#defineXGet_IntrOffset(IntId)((XIsIntrTypeSGI(IntId)==1)?0:((XGet_IntrType(IntId)==1)?16:32))'
    if expected not in offset:raise ValueError('Unknown BSP GIC interrupt offset macro')
    result={}
    for name in ('dma_spectrum','dma_pcm'):
        prefix='XPAR_'+name.upper()
        value=int(define(text,prefix+'_INTERRUPTS').rstrip('UuLl'),0)
        expected=wiring['interrupts'][name]
        if value!=expected['sdt_encoded'] or (value&0xfff)+32!=expected['gic_id'] or (value>>12)&15!=4:raise ValueError('BSP IRQ mapping mismatch: '+name)
        for field in ('base','high'):
            if int(define(text,prefix+('_BASEADDR' if field=='base' else '_HIGHADDR')).rstrip('UuLl'),0)!=wiring['registers'][name][field]:raise ValueError('BSP MMIO mapping mismatch: '+name)
        result[name]={'sdt_encoded':value,'gic_id':(value&0xfff)+32,'trigger':(value>>12)&15}
    result['scope']='GENERATED_BSP_HEADER_FIELDS_ONLY'
    result['header_sha256']=hashlib.sha256(header.read_bytes()).hexdigest()
    result['wrapper_sha256']=hashlib.sha256(wrapper.read_bytes()).hexdigest()
    return result

def validate(path):
    with zipfile.ZipFile(path) as z:
        names=z.namelist()
        if len(names)!=len(set(names)) or any(n.endswith(('.bit','.elf')) for n in names):raise ValueError('Invalid candidate payload')
        hwh_names=[n for n in names if n.endswith('.hwh')]
        root=ET.fromstring(z.read('monitor_dma.hwh'))
        hp=one(root,"./MODULES/MODULE[@INSTANCE='hp0']")
        expected_hwh={'monitor_dma.hwh','monitor_dma_control_connect_0.hwh'}
        if hp.get('MODTYPE')=='smartconnect':expected_hwh|={'monitor_dma_hp0_0.hwh','monitor_dma_hp1_0.hwh'}
        elif hp.get('MODTYPE')!='axi_interconnect':raise ValueError('Unsupported HP interconnect')
        if set(hwh_names)!=expected_hwh:raise ValueError('Unexpected top/hierarchical HWH set')
        if root.get('VIVADOVERSION')!='2026.1':raise ValueError('Wrong Vivado metadata version')
        device=ET.fromstring(z.read('xsa.xml')).find('.//Device')
        if device is None or device.get('FpgaDevice')!='zynq:xc7z020:clg484:-2':raise ValueError('Wrong device')
        modules=root.findall('.//MODULE')
        params=parameters
        dmas=[m for m in modules if m.get('MODTYPE')=='axi_dma']
        if len(dmas)!=2 or {m.get('INSTANCE') for m in dmas}!={'dma_spectrum','dma_pcm'}:raise ValueError('Require two named DMA instances')
        for dma in dmas:check_dma(params(dma))
        ps=[m for m in modules if m.get('MODTYPE')=='processing_system7']
        if len(ps)!=1:raise ValueError('Require PS7')
        fields=params(ps[0])
        for key in ['PCW_EN_DDR','PCW_USE_M_AXI_GP0','PCW_USE_S_AXI_HP0','PCW_USE_S_AXI_HP1','PCW_EN_TTC0']:
            if fields.get(key)!='1':raise ValueError('Missing PS interface '+key)
        for key in ['PCW_EN_QSPI','PCW_EN_SDIO0','PCW_EN_SDIO1','PCW_EN_USB0','PCW_EN_USB1','PCW_EN_ENET0','PCW_EN_ENET1','PCW_EN_UART0','PCW_EN_UART1']:
            if fields.get(key)!='0':raise ValueError('Unexpected peripheral '+key)
        if abs(float(fields['PCW_FPGA0_PERIPHERAL_FREQMHZ'])-50)>1e-6:raise ValueError('Wrong FCLK0')
        plan=memory_plan(int(fields['PCW_DDR_RAM_BASEADDR'],0),int(fields['PCW_DDR_RAM_HIGHADDR'],0))
        wiring=topology(root,int(fields['PCW_DDR_RAM_BASEADDR'],0),int(fields['PCW_DDR_RAM_HIGHADDR'],0),plan)
        spec=importlib.util.spec_from_file_location('ps_init_review',Path(__file__).resolve().parents[1]/'tests/python/test_ps_hardware.py')
        review=importlib.util.module_from_spec(spec);spec.loader.exec_module(review)
        summaries={};risks=[]
        for name in ['ps7_init.c','ps7_init_gpl.c','ps7_init.tcl']:
            if name not in names:raise ValueError('Missing initialization format '+name)
            summary,found=review.inspect_init(name,z.read(name).decode('utf-8'));summaries[name]=summary;risks.extend(found)
            if summary['analysis_status']!='KNOWN':raise ValueError('Unknown initialization structure')
    return {'schema_version':1,'status':'PASS','evidence':'ACTUAL_XSA_CANDIDATE_METADATA_ONLY',
            'board_execution_authorized':False,'init_execution_authorized':False,
            'board_profile_status':'UNVERIFIED_FITTED_DDR_AND_REVISION',
            'xsa_sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest(),
            'hardware_profile':wiring['profile'],
            'runtime_flow_abi':'VERIFIED_FLOW_SINGLE_ENGINE_AND_FOUR_SEQUENCE_BASELINES' if wiring['profile']=='CLOSED_TEST_PATTERN_CANDIDATE' else None,
            'dmas':{m.get('INSTANCE'):params(m) for m in dmas},'memory_plan':plan,
            'init_summary':summaries,'init_risks':risks,'fclk0_hz':50000000,'topology':wiring}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('xsa',type=Path);p.add_argument('--output',type=Path,required=True);p.add_argument('--bsp-include',type=Path);a=p.parse_args()
    record={'status':'RUNNING','evidence':'ACTUAL_XSA_CANDIDATE_METADATA_ONLY',
            'board_execution_authorized':False,'init_execution_authorized':False,
            'validator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    a.output.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    try:
        record.update(validate(a.xsa))
        if a.bsp_include:record['bsp_interrupts']=check_bsp_interrupts(a.bsp_include,record['topology'])
    except Exception as error:
        record.update(status='FAIL',error_type=type(error).__name__)
        a.output.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
        raise
    a.output.write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    print('PASS dma_hardware CANDIDATE_BUILD_ONLY')

if __name__=='__main__':main()
