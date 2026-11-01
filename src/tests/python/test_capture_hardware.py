"""Negative topology controls derived from the actual candidate HWH, no hardware."""
import copy
import importlib.util
import sys
import zipfile
from pathlib import Path
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
import check_capture_hardware as check

def run(xsa):
    record=check.validate(xsa)
    assert record['status']=='PASS' and record['board_execution_authorized'] is False and record['init_execution_authorized'] is False
    with zipfile.ZipFile(xsa) as z:base=ET.fromstring(z.read('capture_dma.hwh'))
    def module(root,name):return check.one(root,f"./MODULES/MODULE[@INSTANCE='{name}']")
    def port(root,inst,name):return check.one(module(root,inst),f"./PORTS/PORT[@NAME='{name}']")
    def param(root,inst,name):return check.one(module(root,inst),f"./PARAMETERS/PARAMETER[@NAME='{name}']")
    cases=[]
    def case(name,edit,reason):cases.append((name,edit,reason))
    case('physical_data_disconnect',lambda r:port(r,'processing','m_spectrum_axis_tdata').set('SIGNAME','wrong_net'),'Disconnected net')
    case('AXIS_both_maps_omit_keep',lambda r:[b.remove(p) for inst,bus in [('processing','m_pcm_axis'),('dma_pcm','S_AXIS_S2MM')] for interface in [check.one(module(r,inst),f"./BUSINTERFACES/BUSINTERFACE[@NAME='{bus}']")] for b in [interface.find('./PORTMAPS')] for p in b.findall('./PORTMAP') if p.get('LOGICAL')=='TKEEP'],'Required bus signal missing')
    case('DMA_sg_disabled',lambda r:param(r,'dma_pcm','C_INCLUDE_SG').set('VALUE','0'),'DMA configuration drift')
    case('wrong_clock_sink',lambda r:port(r,'processing','clk').set('SIGNAME','wrong_clock'),'Disconnected net')
    case('wrong_reset_sink',lambda r:port(r,'processing','aresetn').set('SIGNAME','wrong_reset'),'Disconnected net')
    case('wrong_warmup_parameter',lambda r:param(r,'processing','ADC_WARMUP_CYCLES').set('VALUE','1'),'Processing parameter drift')
    case('wrong_IRQ_net',lambda r:port(r,'irqs','In0').set('SIGNAME','wrong_irq'),'Disconnected net')
    case('wrong_IRQ_trigger',lambda r:port(r,'dma_pcm','s2mm_introut').set('SENSITIVITY','EDGE_RISING'),'IRQ trigger/shape')
    case('missing_audio_reference',lambda r:check.one(r,"./EXTERNALPORTS/PORT[@NAME='ref_clk_in']").set('SIGNAME','wrong_ref'),'Physical audio port disconnected')
    case('wrong_platform_module',lambda r:module(r,'processing').set('MODTYPE','monitor_service_top'),'Wrong candidate module set')
    def overlap(r):
        rs=module(r,'ps7').findall('./MEMORYMAP/MEMRANGE');banks=[x for x in rs if x.get('INSTANCE')=='processing'];assert len(banks)==3
        for k in ('BASEVALUE','HIGHVALUE'):banks[1].set(k,banks[0].get(k))
    case('overlapping_banks',overlap,'Overlapping MMIO')
    def wrong_dma_range(r):
        m=module(r,'dma_pcm').find('./MEMORYMAP/MEMRANGE');m.set('HIGHVALUE','0x001fffff')
    case('DDR_truncated',wrong_dma_range,'DMA DDR bounds/path')
    for name,edit,reason in cases:
        root=copy.deepcopy(base);edit(root)
        try:check.topology(root)
        except ValueError as e:
            assert reason in str(e),(name,str(e));print('PASS reject '+name)
        else:raise AssertionError('Accepted corrupted topology '+name)
    # The old strict checker must not accept a new profile under the legacy ABI.
    import check_dma_hardware as legacy
    try:legacy.validate(xsa)
    except (ValueError,KeyError):pass
    else:raise AssertionError('Legacy checker accepted new profile')
    print('PASS capture_hardware_controls 12 ACTUAL_HWH_MUTATIONS')
if __name__=='__main__':
    if len(sys.argv)!=2:raise SystemExit('Pass actual capture_dma.xsa')
    run(Path(sys.argv[1]))
