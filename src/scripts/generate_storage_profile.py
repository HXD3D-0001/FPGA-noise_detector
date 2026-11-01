"""Compile the current software DSP profile, without any hardware/event facts."""
import hashlib,json,math,re,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'build/storage_profile';out.mkdir(parents=True,exist_ok=True)
p=json.loads((ROOT/'generated/parameters.json').read_text(encoding='utf-8'))
header=(ROOT/'generated/fir_coefficients.h').read_text(encoding='utf-8')
stages=[]
for k,s in enumerate(p['stages'],1):
 values=list(map(int,re.search(r'lfn_fir_stage'+str(k)+r'\[\d+\] = \{([^}]+)',header)[1].split(',')))
 assert len(values)==s['taps'] and hashlib.sha256(b''.join(struct.pack('<i',x) for x in values)).hexdigest()==s['sha256_le_int32']
 stages.append(dict(decimation=s['decimation'],phase=s['phase'],taps=s['taps'],values=values,sha256=s['sha256_le_int32']))
hann=[int(x,16) for x in (ROOT/'generated/hann4096.mem').read_text(encoding='utf-8').split()]
hann=[x if x<0x20000 else x-0x40000 for x in hann]
assert len(hann)==4096 and hashlib.sha256(b''.join(struct.pack('<i',x) for x in hann)).hexdigest()==p['hann']['sha256_le_int32']
profile=dict(algorithm_hash=hashlib.sha256((ROOT/'ps/dsp/spectrum.c').read_bytes()).hexdigest(),rom_manifest_hash=hashlib.sha256((ROOT/'generated/parameters.json').read_bytes()).hexdigest(),window_hash=p['hann']['sha256_le_int32'],raw_fs_hz=48000,processed_fs_hz=1000,group_delay_raw_ticks=831,fft_count=4096,fft_hop=2048,psd_bins=1311,psd_unit='FS2_PER_HZ',window_definition='PERIODIC_HANN_Q1_17',window_sum_squares=sum((x/131072)**2 for x in hann),coefficients=dict(format='SIGNED_Q1_17',rounding='NEAREST_TIES_AWAY',accumulator_bits=56,stages=stages),bands=[[1000*10**((i-15)/10)/10**0.05,1000*10**((i-15)/10)*10**0.05] for i in range(10)],target_low_hz=31.5,target_high_hz=250)
profile['summary_histogram']=dict(reference_power_fs2=0.5,min_dbfs=-120,step_db=5,bin_count=26)
raw=json.dumps(profile,separators=(',',':'),allow_nan=False)
(out/'storage_profile.h').write_text('#ifndef LFN_STORAGE_PROFILE_H\n#define LFN_STORAGE_PROFILE_H\nstatic const char lfn_storage_profile[] = '+json.dumps(raw[1:-1])+';\n#endif\n',encoding='utf-8')
(out/'profile.json').write_text(raw+'\n',encoding='utf-8')
print('PASS storage_profile ACTUAL_FIR_HANN_ALGORITHM_NO_HARDWARE_FACTS')
