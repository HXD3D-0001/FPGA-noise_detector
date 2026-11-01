"""Inspect candidate DDR image ELF/map; never authorize or execute it."""
import argparse
import json
from pathlib import Path
import re
import struct

def inspect_loads(data,app_base,app_end,data_base,data_end):
    if not (0<app_base<app_end<=data_base<data_end<=2**32) or data_end-data_base!=32*1024*1024:
        raise ValueError('Invalid separated app/data memory plan')
    if len(data)<52 or data[:7]!=b'\x7fELF\x01\x01\x01':raise ValueError('Require ELF32 little endian version1')
    h=struct.unpack_from('<HHIIIIIHHHHHH',data,16)
    typ,machine,version,entry,phoff,_,flags,ehsize,phsize,phnum,_,_,_=h
    if typ!=2 or machine!=40 or version!=1 or ehsize!=52 or phsize!=32 or not phnum:
        raise ValueError('Require linked ARM executable with program headers')
    if flags&0xff000000!=0x05000000 or not flags&0x400 or flags&0x200:raise ValueError('Require ARM EABI5 hard-float candidate')
    if not app_base<=entry<app_end:raise ValueError('Entry outside candidate app region')
    if phoff+phsize*phnum>len(data):raise ValueError('Truncated program headers')
    loads=[]
    for i in range(phnum):
        kind,offset,virtual,physical,filesz,memsz,permissions,alignment=struct.unpack_from('<IIIIIIII',data,phoff+phsize*i)
        if kind!=1 or not memsz:continue
        if virtual!=physical or filesz>memsz or offset+filesz>len(data) or physical+memsz>2**32:
            raise ValueError('Invalid nonidentity/truncated load segment')
        end=physical+memsz
        if app_base<=physical and end<=app_end:region='app'
        elif data_base<=physical and end<=data_end and filesz==0:region='dedicated_noload'
        else:raise ValueError('Load overlaps or escapes reserved app/data regions')
        if any(physical<x['end'] and x['address']<end for x in loads):raise ValueError('Overlapping program segments')
        loads.append({'address':physical,'end':end,'file_bytes':filesz,'memory_bytes':memsz,'flags':permissions,'alignment':alignment,'region':region})
    if not any(x['address']<=entry<x['end'] and x['flags']&1 and x['file_bytes'] for x in loads):
        raise ValueError('Entry not in initialized executable segment')
    return {'entry':entry,'loads':loads,'abi':'EABI5_HARD_FLOAT_BUILD_ONLY','board_execution_authorized':False}

def inspect_symbols(text,plan,loads=None,entry=None,*,profile="legacy"):
    symbols={}
    for line in text.splitlines():
        match=re.fullmatch(r'([0-9a-fA-F]+)\s+(?:([0-9a-fA-F]+)\s+)?([A-Za-z])\s+(\S+)',line.strip())
        if match:symbols[match[4]]=(int(match[1],16),int(match[2],16) if match[2] else 0)
    required={'main','_vector_table','_boot','_start','MMUTable','vTaskStartScheduler','vPortTaskUsesFPU','capture_task_run','lfn_analysis_step','XAxiDma_BdRingToHw','XAxiDma_BdRingFromHw','lfn_runtime_prepare_capture'}
    if profile == "capture":
        required.remove("lfn_runtime_prepare_capture")
        required.update({"lfn_capture_image_rotation_port", "lfn_capture_image_cold_port", "lfn_capture_image_control_submit", "lfn_capture_image_control_attach", "lfn_capture_image_control_copy", "lfn_capture_image_control_ack", "lfn_capture_image_control_barrier", "capture_control_step", "capture_control_owner_paused", "capture_application_resume_storage", "capture_application_step", "capture_runtime_step", "capture_data_owner_step",
            "lfn_capture_image_request_start", "lfn_capture_image_request_stop", "pl_contract_bind",
            "pl_capture_commit_request", "pl_data_energy_read", "dma_rx_xilinx_rtos_connect_sdt",
            "lfn_capture_image_storage_attach", "lfn_capture_image_storage_close", "capture_storage_route_packet", "capture_storage_route_psd", "lfn_service_consumer_claim", "lfn_capture_image_snapshot_copy", "lfn_capture_image_energy_take", "lfn_service_energy_offer", "lfn_service_snapshot_publish", "lfn_capture_image_raw_read", "lfn_raw_offer"})
        if {"lfn_runtime_prepare_capture", "runtime_control_step", "lfn_runtime_request_test"} & symbols.keys():
            raise ValueError("Legacy control entry linked into capture image")
    elif profile != "legacy":
        raise ValueError("Unknown runtime symbol profile")
    if not required<=symbols.keys():raise ValueError('Missing real BSP/RTOS/SG/service symbols: '+repr(sorted(required-symbols.keys())))
    if 'ps7_init' in symbols or 'ps7_config' in symbols:raise ValueError('DDR hardware initializer must not be linked into runtime candidate')
    memory={}
    fixed=[('lfn_fft_dma',32*65536),('lfn_pcm_dma',128*2048),('lfn_fft_bd',32*64),('lfn_pcm_bd',128*64),('lfn_packet_pool',8*32832)]
    if profile == 'capture':fixed.append(('lfn_raw_buffer',8*1024*1024))
    for name,minsize in fixed:
        if name not in symbols:raise ValueError('Missing fixed memory '+name)
        address,size=symbols[name]
        if address%64 or size<minsize or not plan['data_base']<=address<address+size<=plan['data_end']:
            raise ValueError('Memory alignment/budget violation: '+name)
        if (name.endswith('_dma') or name=='lfn_raw_buffer') and size!=minsize:raise ValueError('Unexpected DMA data budget')
        memory[name]={'address':address,'size':size}
    stride=memory['lfn_fft_bd']['size']//32
    if memory['lfn_fft_bd']['size']%32 or stride%64 or memory['lfn_pcm_bd']['size']!=128*stride:
        raise ValueError('BD stride inconsistent with160 generated descriptors')
    ranges=list(memory.values())
    for i,a in enumerate(ranges):
        for b in ranges[i+1:]:
            if a['address']<b['address']+b['size'] and b['address']<a['address']+a['size']:raise ValueError('Fixed memory objects overlap')
    for name in ['__stack','__irq_stack','__supervisor_stack','__abort_stack','__fiq_stack','__undef_stack','_heap','_heap_end']:
        if name not in symbols or not plan['app_base']<=symbols[name][0]<plan['app_end']:raise ValueError('Missing/outside app stack/heap: '+name)
    if symbols['MMUTable'][0]%65536:raise ValueError('MMU table not64KiB aligned')
    if loads is not None:
        if entry!=symbols['_vector_table'][0]:raise ValueError('Entry is not the actual BSP vector')
        mmu,_=symbols['MMUTable']
        if '__mmu_tbl_start' not in symbols or '__mmu_tbl_end' not in symbols or symbols['__mmu_tbl_start'][0]!=mmu:raise ValueError('Missing MMU section bounds')
        size=symbols['__mmu_tbl_end'][0]-mmu
        if size!=16384 or not any(x['region']=='app' and x['address']<=mmu and mmu+size<=x['address']+x['file_bytes'] for x in loads):
            raise ValueError('MMU table must be16KiB inside initialized app load')
        for name,obj in memory.items():
            if not any(x['region']=='dedicated_noload' and x['address']<=obj['address'] and obj['address']+obj['size']<=x['end'] for x in loads):
                raise ValueError('Fixed memory not covered by dedicated NOLOAD load: '+name)
    return {'fixed_memory':memory,'bd_stride':stride,'descriptor_count':160,'symbols_verified':sorted(required)}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ['elf','symbols','attributes','validation','output']:parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();receipt=json.loads(args.validation.read_text(encoding='utf-8-sig'))
    if receipt['status']!='PASS' or receipt['board_execution_authorized'] or receipt['init_execution_authorized']:
        raise ValueError('Require current build-only candidate validation')
    result=inspect_loads(args.elf.read_bytes(),**{k:receipt['memory_plan'][k] for k in ['app_base','app_end','data_base','data_end']})
    result.update(inspect_symbols(args.symbols.read_text(encoding='utf-8-sig'),receipt['memory_plan'],result['loads'],result['entry']))
    attrs=args.attributes.read_text(encoding='utf-8-sig')
    if 'Tag_ABI_VFP_args: VFP registers' not in attrs:raise ValueError('Missing verified hard-float VFP argument attribute')
    result.update(status='PASS',evidence='ACTUAL_GNU_GENERATED_FREERTOS_DMA_BSP_LINK_ONLY',physical_dma='NOT_RUN',scheduler='NOT_RUN',source_startup='NOT_RUN',ddr_init='NOT_RUN')
    args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PASS dma_runtime_elf BUILD_ONLY')
