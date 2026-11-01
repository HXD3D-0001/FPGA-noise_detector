"""Bind real FFT RTL/packet evidence to the generated IP's official C model."""
from __future__ import annotations
import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile
import numpy as np

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/model_validation'))
import check_fft_model as model
N=4096
SEQUENCES=(0,1,2,4)

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def validate_timing(lines):
    timing=[];previous_end=-1
    for expected_index,line in enumerate(lines):
        if expected_index>=len(SEQUENCES):raise AssertionError('Extra timing record')
        fields=line.split()
        if len(fields)!=7:raise AssertionError('Timing record field count')
        packet,ready,offer,last,end,ingest,job=map(int,fields)
        if packet!=expected_index:raise AssertionError('Timing packet order/identity')
        if not previous_end<ready<=offer<last<=end:raise AssertionError('Timing chronology')
        if not 0<=offer-ready<=10:raise AssertionError('Window prefetch margin')
        if ingest!=last-offer+1:raise AssertionError('Ingest counter/trace mismatch')
        if not ingest<=job<=end-offer+1:raise AssertionError('Job counter/trace mismatch')
        if not 0<last-ready<50000 or not 0<ingest<49990 or not 0<job<5000000:raise AssertionError('FFT deadline violated')
        timing.append({'seq':SEQUENCES[packet],'bank_release_cycles':last-ready,'ingest_cycles':ingest,
                       'job_cycles':job,'packet_latency_cycles':end-ready,'prefetch_cycles':offer-ready})
        previous_end=end
    if len(timing)!=4:raise AssertionError('Missing job timing')
    return timing

def generate(build,xci):
    contract=model.validate_xci(xci)
    instance=json.loads(xci.read_text(encoding='utf-8'))['ip_inst']
    if instance.get('ip_revision')!='16':raise ValueError('FFT IP revision must be 16')
    widths={'s_axis_config_tdata':7,'s_axis_data_tdata':47,'m_axis_data_tdata':47,
            'm_axis_data_tuser':23,'m_axis_status_tdata':7}
    for name,left in widths.items():
        records=instance['boundary']['ports'][name]
        if len(records)!=1 or records[0].get('size_left')!=str(left) or records[0].get('size_right')!='0':
            raise ValueError(f'FFT interface width drift {name}')
    samples=np.arange(12288,dtype=np.float64)
    rng=np.random.default_rng(7021)
    signal=sum(amplitude*np.sin(2*np.pi*frequency*samples/1000+phase)
               for frequency,amplitude,phase in ((31.5,.11,.17),(63,.09,.41),
                   (125,.07,.63),(250,.05,.89)))+rng.normal(0,.003,samples.size)
    pcm=model.quantize(signal)
    pcm[5500]=8388607;pcm[5501]=-8388608;pcm[10500:10564:2]=8388607
    pcm[10501:10564:2]=-8388608
    (build/'fft_samples.mem').write_text(''.join(f'{int(x)&0xffffff:06x}\n' for x in pcm),encoding='ascii')
    hann=np.array([int(line,16) for line in (ROOT/'generated/hann4096.mem').read_text(encoding='ascii').splitlines()],dtype=np.int64)
    if len(hann)!=N:raise ValueError('Hann coefficient count')
    zip_path=xci.parent/'cmodel/xfft_v9_1_bitacc_cmodel_nt64.zip'
    handles=[];dll=None;state=None
    report={'status':'RUNNING','evidence':'Actual generated FFT RTL compared with matched official C model and shared C wire codec',
            'exclusions':['timing closure','board FFT/DMA','microphone/acoustic calibration'],
            'xci_sha256':digest(xci),'model_zip_sha256':digest(zip_path),
            'hann_sha256':digest(ROOT/'generated/hann4096.mem'),'seed':7021,'frames':[]}
    try:
        with tempfile.TemporaryDirectory(prefix='lf_fft_pipeline_') as temporary:
            with zipfile.ZipFile(zip_path) as archive:
                for name in (*model.DEPENDENCIES,'libIp_xfft_v9_1_bitacc_cmodel.dll'):
                    data=archive.read(name);Path(temporary,name).write_bytes(data)
                    if name.startswith('libIp_xfft'):report['model_dll_sha256']=hashlib.sha256(data).hexdigest()
            handles.append(os.add_dll_directory(temporary))
            dll=ct.CDLL(str(Path(temporary,'libIp_xfft_v9_1_bitacc_cmodel.dll')))
            model.bind(dll)
            generics=model.Generics(*(contract['generics'][name] for name in model.GENERIC_NAMES))
            state=dll.xilinx_ip_xfft_v9_1_create_state(generics)
            if not state:raise RuntimeError('FFT C model state creation failed')
            for seq in range(5):
                window=model.window_pcm(pcm[seq*2048:seq*2048+N],hann)
                _,_,mantissa,metadata=model.simulate(dll,state,window)
                values=np.column_stack((np.rint(mantissa.real*model.SCALE),np.rint(mantissa.imag*model.SCALE))).astype('<i4')
                payload=build/f'fft_gold_{seq}.values';payload.write_bytes(values.tobytes())
                (build/f'fft_window_{seq}.mem').write_text(''.join(f'{int(x)&0xffffff:06x}\n' for x in window),encoding='ascii')
                report['frames'].append({'seq':seq,'first_raw':seq*2048*48,'blk_exp':metadata['blk_exp'],
                    'flags':5 if seq==4 else 1,'values_sha256':digest(payload),'window_sha256':digest(build/f'fft_window_{seq}.mem')})
            dll.xilinx_ip_xfft_v9_1_destroy_state(state);state=None
            import _ctypes
            _ctypes.FreeLibrary(dll._handle);dll=None
    finally:
        if dll is not None:
            if state:dll.xilinx_ip_xfft_v9_1_destroy_state(state)
            import _ctypes
            _ctypes.FreeLibrary(dll._handle)
        for handle in handles:handle.close()
    report['status']='GOLD_READY'
    (build/'fft_gold.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('PASS fft_pipeline_vectors')

def check(build,encoder):
    report=json.loads((build/'fft_gold.json').read_text(encoding='utf-8'))
    rows=(build/'fft_packets.txt').read_text(encoding='ascii').splitlines()
    packets=[];current=[]
    for line in rows:
        fields=line.split()
        if len(fields)!=2 or len(fields[0])!=16 or fields[1] not in ('0','1'):raise AssertionError('Malformed packet trace')
        current.append(int(fields[0],16).to_bytes(8,'little'))
        if fields[1]=='1':
            if len(current)!=4104:raise AssertionError('Premature/late TLAST')
            packets.append(b''.join(current));current=[]
    if current or len(packets)!=4:raise AssertionError('Expected four complete spectra')
    for packet,seq in zip(packets,SEQUENCES):
        frame=report['frames'][seq];gold=build/f'fft_gold_{seq}.packet'
        completed=subprocess.run([str(encoder.resolve()),str((build/f'fft_gold_{seq}.values').resolve()),str(gold.resolve()),
            str(seq),str(frame['first_raw']),str(frame['flags']),str(frame['blk_exp'])],check=True,capture_output=True,text=True)
        if completed.stdout.strip()!='PASS fft_packet_gold':raise AssertionError('C codec marker missing')
        expected=gold.read_bytes()
        if packet!=expected:
            first=next((i for i,(a,b) in enumerate(zip(packet,expected)) if a!=b),min(len(packet),len(expected)))
            raise AssertionError(f'FFT frame {seq} complete packet mismatch at byte {first}; actual/expected {packet[first:first+16].hex()}/{expected[first:first+16].hex()}')
        frame['packet_sha256']=hashlib.sha256(packet).hexdigest()
        print(f'PASS fft_complex_packet seq={seq} bins=4096 blk_exp={frame["blk_exp"]} bytes={len(packet)}')
    # Input, including the locally drained failed frame, must match exact Q1.17 Hann arithmetic.
    positions={seq:0 for seq in range(5)}
    expected_windows={seq:[int(x,16) for x in (build/f'fft_window_{seq}.mem').read_text(encoding='ascii').splitlines()] for seq in range(5)}
    for line in (build/'fft_window.txt').read_text(encoding='ascii').splitlines():
        seq,index,value=line.split();seq=int(seq);index=int(index)
        if seq not in positions or index!=positions[seq] or int(value,16)!=expected_windows[seq][index]:raise AssertionError('Window/index mismatch')
        positions[seq]+=1
    if any(x!=N for x in positions.values()):raise AssertionError(f'Incomplete window/drain trace {positions}')
    timing=validate_timing((build/'fft_timing.txt').read_text(encoding='ascii').splitlines())
    report['timing']=timing;report['status']='PASS';report['sample_sha256']=digest(build/'fft_samples.mem')
    (build/'fft_pipeline_result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('PASS fft_pipeline_wire_and_model')

def main():
    parser=argparse.ArgumentParser();parser.add_argument('action',choices=('generate','check'))
    parser.add_argument('--build',type=Path,required=True);parser.add_argument('--xci',type=Path)
    parser.add_argument('--encoder',type=Path);args=parser.parse_args()
    args.build.mkdir(parents=True,exist_ok=True)
    if args.action=='generate':generate(args.build,args.xci)
    else:check(args.build,args.encoder)
if __name__=='__main__':main()
