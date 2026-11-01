"""Inspect actual target resource sizes and call-graph evidence, never execute."""
import re,struct
from pathlib import Path

def inspect_storage(data,text,plan,base,output,units):
 symbols={}
 for line in text.splitlines():
  m=re.fullmatch(r'([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([A-Za-z])\s+(\S+)',line.strip())
  if m:symbols[m[4]]=(int(m[1],16),int(m[2],16))
 required=['lfn_storage_task_'+x for x in ('connect_capture','source_facts','start','start_sd','sd_closed','metadata','energy','psd','pcm','poll','ack','abort','close','recover','health')]+['capture_storage_route_step','lfn_service_energy_take_owned','storage_pump_step','storage_service_step','storage_registry_validate','lfn_storage_resources','lfn_storage_resource_sizes','storage_owner_prepare','storage_owner_attach_ports','lfn_storage_owner_freertos_mount','lfn_storage_owner_freertos_unmount']
 if any(x not in symbols for x in required):raise ValueError('Missing real storage task symbols')
 address,size=symbols['lfn_storage_resources']
 if address%64 or not plan['data_base']<=address<address+size<=plan['data_end']:raise ValueError('Storage resource alignment/budget')
 if not any(x['region']=='dedicated_noload' and x['address']<=address and address+size<=x['end'] for x in base['loads']):raise ValueError('Storage resources must be NOLOAD')
 for name in ('lfn_fft_dma','lfn_pcm_dma','lfn_fft_bd','lfn_pcm_bd','lfn_packet_pool','lfn_raw_buffer'):
  a,n=symbols[name]
  if address<a+n and a<address+size:raise ValueError('Storage/DMA/RAW overlap')
 a,n=symbols['lfn_storage_resource_sizes'];phoff=struct.unpack_from('<I',data,28)[0];phnum=struct.unpack_from('<H',data,44)[0];raw=None
 for k in range(phnum):
  kind,off,v,_,filesz,_,_,_=struct.unpack_from('<8I',data,phoff+32*k)
  if kind==1 and v<=a and a+n<=v+filesz:raw=data[off+a-v:off+a-v+n]
 manifest=('total','registry','writer','queue','service','builder','pump','stack_word','rtos_heap','storage_stack_words','idle_stack_words','timer_stack_words','route','fatfs','fil','volume','owner','owner_fs')
 if raw is None or len(raw)!=4*len(manifest):raise ValueError('Missing actual ARM size manifest')
 sizes=dict(zip(manifest,struct.unpack('<'+str(len(manifest))+'I',raw)))
 if sizes['total']!=size or sizes['stack_word']!=4 or sizes['storage_stack_words']!=6144 or sizes['rtos_heap']!=65536:raise ValueError('Unexpected target ABI/heap/stack contract')
 if sizes['owner']<13*1024*1024 or sizes['owner']>16*1024*1024:raise ValueError('Owner workspace instance outside budget')
 if sizes['owner_fs']<256 or sizes['owner_fs']>49152:raise ValueError('Owner FatFs glue instance outside budget')
 if not sizes['fatfs'] or not sizes['fil'] or not sizes['volume']:raise ValueError('Missing task-owned FatFs object sizes')
 known_stacks=(4096+2048+sizes['storage_stack_words']+sizes['idle_stack_words']+sizes['timer_stack_words'])*4
 if known_stacks+8192>sizes['rtos_heap']:raise ValueError('Candidate task stacks and control margin exceed BSP heap')
 su=list(Path(output).glob('*.su'));ci=list(Path(output).glob('*.ci'))
 if len(su)!=units or len(ci)!=units:raise ValueError(f'Require stack and callgraph for all{units} production translation units')
 if not (Path(output)/'ps_app_capture_control.c.su').is_file() or not (Path(output)/'ps_app_capture_control.c.ci').is_file():raise ValueError('Require real capture control stack/callgraph reports')
 if not (Path(output)/'ps_app_storage_rotation_port.c.su').is_file() or not (Path(output)/'ps_app_storage_rotation_port.c.ci').is_file():raise ValueError('Require real rotation port stack/callgraph reports')
 frames={}
 for p in su:
  for line in p.read_text(encoding='utf-8').splitlines():
   fields=line.split('\t')
   if len(fields)>=3:frames[fields[0]]=dict(bytes=int(fields[1]),kind=fields[2])
 nodes={};edges={}
 for p in ci:
  graph=p.read_text(encoding='utf-8')
  for m in re.finditer(r'node: \{ title: "(.*?)" label: "(.*?)"',graph):
   key,label=m.groups();frame=re.search(r'(\d+) bytes [(](.*?)[)]',label)
   if frame or key not in nodes:nodes[key]=dict(name=label.split(chr(92)+'n')[0],frame=int(frame[1]) if frame else None)
  for m in re.finditer(r'edge: \{ sourcename: "(.*?)" targetname: "(.*?)"',graph):edges.setdefault(m[1],set()).add(m[2])
 unknown=set()
 def walk(key,seen):
  node=nodes.get(key,dict(name=key,frame=None))
  if node['frame'] is None:unknown.add(node['name']);return (0,[])
  count=seen.count(key)
  if count:
   if ':value' not in key:unknown.add('UNBOUNDED_RECURSION:'+node['name']);return(0,[])
   if count>=14:return(0,[]) # depth0..12 plus rejecting depth13 frame
  options=[walk(x,seen+(key,)) for x in edges.get(key,())]
  best=max(options or [(0,[])],key=lambda x:x[0])
  return(node['frame']+best[0],[dict(function=node['name'],frame=node['frame'])]+best[1])
 roots=[k for k in nodes if k.endswith('storage_task_freertos.c:entry')]
 chains=[walk(k,()) for k in roots]
 if not chains:raise ValueError('No actual storage task callgraph root')
 chain=max(chains,key=lambda x:x[0])
 capture_roots=[k for k in nodes if k.endswith('capture_image_main.c:capture_entry')]
 if not capture_roots:raise ValueError('No actual capture task callgraph root')
 capture_chain=max([walk(k,()) for k in capture_roots],key=lambda x:x[0])
 # Full finite call-chain bound cannot be asserted: libc, FreeRTOS archives,
 # indirect backend callbacks and bounded recursive JSON parser remain external.
 return dict(address=address,bytes=size,sizes=sizes,known_stack_allocations_bytes=known_stacks,control_margin_bytes=8192,heap_unallocated_after_candidate_margin=sizes['rtos_heap']-known_stacks-8192,stack_frames=frames,known_source_chain_frame_sum=chain[0],known_source_chain=chain[1],capture_known_source_chain_frame_sum=capture_chain[0],capture_known_source_chain=capture_chain[1],unresolved_callees=sorted(unknown),stack_reports=len(su),callgraph_reports=len(ci),full_call_chain_bound='UNRESOLVED_LIBC_BSP_BACKEND_RECURSION',task_stack_bytes=24576,physical_ddr='NOT_VERIFIED',scheduler='NOT_RUN',backend='EXPLICIT_BACKEND_OR_TASK_OWNED_FATFS',runtime_high_water='API_PROVIDED_NOT_MEASURED')