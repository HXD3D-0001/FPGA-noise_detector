"""Build the vendor Zynq FSBL and package BOOT.BIN offline; never program media."""
import argparse,hashlib,json,os,shutil,subprocess,struct,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--suite-root',type=Path,required=True);p.add_argument('--image-receipt',type=Path,default=ROOT/'build/capture_network/current/status.json');p.add_argument('--bitstream',type=Path,default=ROOT/'build/pre_adc_implementation/pre_adc_impl.bit');p.add_argument('--output',type=Path,default=ROOT/'build/product_boot');a=p.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);logs=out/'logs';logs.mkdir(exist_ok=True);src=out/'src';src.mkdir(exist_ok=True);inc=out/'include';inc.mkdir(exist_ok=True);lib=out/'lib';lib.mkdir(exist_ok=True);obj=out/'obj';obj.mkdir(exist_ok=True)
 status={'status':'RUNNING','scope':'FSBL_AND_BOOT_BIN_OFFLINE_BUILD_ONLY','media_written':False,'board_boot':'NOT_RUN','source_hashes':{},'commands':{}}
 def save():(out/'status.json').write_text(json.dumps(status,indent=2)+'\n',encoding='utf-8')
 def track(path):path=Path(path).resolve();status['source_hashes'][str(path)]=digest(path);return path
 def copy(path,dest):track(path);shutil.copyfile(path,dest);return dest
 suite=a.suite_root.resolve();bin=suite/'gnu/aarch32/nt/gcc-arm-none-eabi/bin';gcc=track(bin/'arm-none-eabi-gcc.exe');bootgen=track(suite/'Vivado/bin/unwrapped/win64.o/bootgen.exe')
 env=dict(os.environ);env['PATH']=str(bin)+os.pathsep+str(suite/'Vivado/lib/win64.o')+os.pathsep+env.get('PATH','');env['TEMP']=env['TMP']=str(out)
 def run(name,args):
  args=[str(x) for x in args];r=subprocess.run(args,cwd=out,env=env,capture_output=True,text=True,errors='replace',timeout=120);(logs/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');status['commands'][name]={'args':args,'exit_code':r.returncode};save()
  if r.returncode:raise RuntimeError(name+' failed: '+(r.stdout+r.stderr)[-1500:])
  return r.stdout
 try:
  track(Path(__file__));receipt=json.loads(track(a.image_receipt).read_text(encoding='utf-8-sig'));assert receipt['status']=='PASS'
  platform=Path(receipt['platform_dir']);bsp=platform/'ps7_cortexa9_0/standalone_cpu0/bsp';rtos=platform/'ps7_cortexa9_0/freertos_cpu0/bsp';xsa=track(platform/'hw/pre_adc_dma.xsa');assert digest(xsa)==receipt['xsa_sha256'].lower()
  app=Path(receipt['elf']);assert digest(app)==receipt['artifacts'][str(app)].lower()
  status['platform']=str(platform);status['xsa_sha256']=digest(xsa);status['application_sha256']=digest(app)
  for h in (bsp/'include').glob('*.h'):copy(h,inc/h.name)
  for name in ('libxil.a','libxilstandalone.a','libxiltimer.a'):copy(bsp/'lib'/name,lib/name)
  for name in ('ff.h','ffconf.h','diskio.h','xilffs_config.h','xilffs.h'):copy(rtos/'include'/name,inc/name)
  # Recompile official FatFs without FREERTOS_BSP; do not link the RTOS archive.
  vendor=suite/'Vitis/data/embeddedsw/lib/sw_apps/zynq_fsbl/src'
  for f in vendor.iterdir():
   if f.suffix in ('.c','.h','.S','.ld'):copy(f,src/f.name)
  with zipfile.ZipFile(xsa) as z:
   for name in ('ps7_init.c','ps7_init.h'):
    member=next(n for n in z.namelist() if n.endswith('/'+name) or n==name);data=z.read(member);assert data==(platform/'hw/sdt'/name).read_bytes();(src/name).write_bytes(data);track(platform/'hw/sdt'/name)
  fat=suite/'Vitis/data/embeddedsw/lib/sw_services/xilffs_v5_6/src'
  for name in ('ff.c','diskio.c','ffsystem.c','ffunicode.c'):copy(fat/name,src/name)
  flags=['-mcpu=cortex-a9','-marm','-mfpu=vfpv3','-mfloat-abi=hard','-DSDT','-DFSBL_DEBUG','-Os','-ffunction-sections','-fdata-sections','-I'+str(inc),'-I'+str(src)]
  objects=[]
  for f in sorted(src.iterdir()):
   if f.suffix not in ('.c','.S'):continue
   o=obj/(f.name+'.o');run('compile_'+f.stem,[gcc,*flags,'-c',f,'-o',o]);objects.append(o)
  crt=[Path(run('find_'+name,[gcc,'-mcpu=cortex-a9','-mfpu=vfpv3','-mfloat-abi=hard','-print-file-name='+name]).strip()) for name in ('crti.o','crtn.o')]
  for f in crt:track(f)
  elf=out/'fsbl.elf';run('link',[gcc,*flags,'-nostartfiles',crt[0],*objects,'-Wl,--build-id=none,--no-undefined,--gc-sections,-u,_vector_table','-Wl,-Map='+str(out/'fsbl.map'),'-T'+str(src/'lscript.ld'),'-Wl,--start-group',lib/'libxil.a',lib/'libxilstandalone.a',lib/'libxiltimer.a','-lc','-lgcc','-Wl,--end-group',crt[1],'-o',elf])
  symbols=run('symbols',[bin/'arm-none-eabi-nm.exe',elf]);run('segments',[bin/'arm-none-eabi-readelf.exe','-l',elf]);run('size',[bin/'arm-none-eabi-size.exe',elf])
  for name in ('_vector_table','ps7_init','ps7_post_config','FsblHandoffExit','InitSD','LoadBootImage'):
   assert name in symbols,'Missing FSBL symbol '+name
  raw=elf.read_bytes();assert raw[:4]==b'\x7fELF' and raw[4]==1;entry,phoff=struct.unpack_from('<II',raw,24);phsize,phnum=struct.unpack_from('<HH',raw,42);segments=[]
  for n in range(phnum):
   kind,offset,vaddr,paddr,filesz,memsz,prot,align=struct.unpack_from('<8I',raw,phoff+n*phsize)
   if kind==1:
    assert (paddr+memsz<=0x30000) or (0xffff0000<=paddr and paddr+memsz<=0xfffffe00),'FSBL outside OCM';segments.append({'address':paddr,'file_bytes':filesz,'memory_bytes':memsz})
  assert entry<0x30000;status['fsbl_segments']=segments;status['entry']=entry
  copy(app,out/'capture_image.elf');copy(a.bitstream,out/'capture.bit')
  bif=out/'boot.bif';bif.write_text('the_ROM_image:\n{\n [bootloader] fsbl.elf\n capture.bit\n capture_image.elf\n}\n',encoding='ascii')
  run('bootgen',[bootgen,'-arch','zynq','-image',bif,'-o',out/'BOOT.BIN','-w','on']);run('bootgen_read',[bootgen,'-arch','zynq','-read',out/'BOOT.BIN'])
  boot=(out/'BOOT.BIN').read_bytes();assert struct.unpack_from('<II',boot,0x20)==(0xaa995566,0x584c4e58)
  assert sum(struct.unpack_from('<11I',boot,0x20))&0xffffffff==0xffffffff
  iht,pht=struct.unpack_from('<II',boot,0x98);count=struct.unpack_from('<I',boot,iht+4)[0];assert count==4
  headers=[struct.unpack_from('<16I',boot,pht+64*k) for k in range(count)]
  assert all(sum(h)&0xffffffff==0xffffffff for h in headers)
  app_raw=app.read_bytes();app_entry,app_phoff=struct.unpack_from('<II',app_raw,24);app_phsize,app_phnum=struct.unpack_from('<HH',app_raw,42)
  app_loads=[struct.unpack_from('<8I',app_raw,app_phoff+k*app_phsize) for k in range(app_phnum)]
  app_loads=[s for s in app_loads if s[0]==1 and s[4]];assert len(app_loads)==2
  for h,s in zip(headers[2:],app_loads):
   assert h[3]==s[3] and boot[h[5]*4:h[5]*4+s[4]]==app_raw[s[1]:s[1]+s[4]]
  assert headers[2][4]==app_entry
  status['boot_validation']={'partition_count':count,'header_checksums':'PASS','application_load_bytes':'EXACT_ELF_MATCH','application_entry':app_entry}
  assert all(digest(path)==h for path,h in status['source_hashes'].items()),'Inputs changed during build'
  status.update(status='PASS',artifacts={str(out/name):digest(out/name) for name in ('fsbl.elf','BOOT.BIN','boot.bif','capture.bit','capture_image.elf')},notes=['Uses unmodified installed official zynq_fsbl sources and matching XSA ps7_init','Standalone BSP archives only; FatFs compiled separately without RTOS','BOOT.BIN contains FSBL, PL bitstream, then current network application','Not copied to SD and not cold-boot tested'])
 except Exception as e:status.update(status='FAIL',error=str(e))
 save();print(status['status'],status.get('error',''));return 0 if status['status']=='PASS' else 1
if __name__=='__main__':raise SystemExit(main())
