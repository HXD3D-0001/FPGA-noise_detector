"""Host-only metadata fixtures using current generated DSP constants.
Hardware/gain IDs are explicitly HOST_FIXTURE/UNVERIFIED, never board claims.
"""
import json, hashlib, re, struct, math
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'build/storage_metadata/fixtures';out.mkdir(parents=True,exist_ok=True)
params=json.loads((ROOT/'generated/parameters.json').read_text())
header=(ROOT/'generated/fir_coefficients.h').read_text()
seq=0
manifest=[]
def save(name,typ,config,capture,first,end,payload):
 global seq
 seq+=1
 if typ==1:
  payload=dict(payload);payload.pop('config_hash',None);payload['config_hash']=hashlib.sha256(json.dumps(payload,sort_keys=True,separators=(',',':')).encode()).hexdigest()
 raw=json.dumps(payload,ensure_ascii=False,separators=(',',':')).encode()
 (out/(name+'.json')).write_bytes(raw)
 manifest.append(f'{name} {typ} {config} {capture} {first} {end} {seq}\n')
def base(config=None,capture=None):return dict(schema_version=1,device_id='HOST_FIXTURE',session_id='9007199254740993',capture_id=str(capture) if capture else None,config_id=config)
session=base();session.update(temporary=True,boot_generation='0',build_manifest_hash=hashlib.sha256(header.encode()).hexdigest(),start_monotonic_ns='9007199254741000');save('session',11,0,0,0,0,session)
config=base(7);config.update(config_hash=hashlib.sha256((ROOT/'generated/parameters.json').read_bytes()).hexdigest(),algorithm_hash=hashlib.sha256((ROOT/'ps/dsp/spectrum.c').read_bytes()).hexdigest(),rom_manifest_hash=hashlib.sha256((ROOT/'generated/parameters.json').read_bytes()).hexdigest(),window_hash=params['hann']['sha256_le_int32'],hardware_id='HOST_FIXTURE',gain_setting_id='UNVERIFIED',adc_id='HOST_FIXTURE',microphone_id='HOST_FIXTURE',point_pose_id='HOST_FIXTURE',gain_status='UNVERIFIED',analog_gain_db=None,mode=0,source=1,channel=0,pattern=0,raw_fs_hz=48000,processed_fs_hz=1000,fs_measured=False,measured_raw_fs_hz=None,fs_uncertainty_ppm=None,group_delay_raw_ticks=831,fft_count=4096,fft_hop=2048,psd_bins=1311,psd_unit='FS2_PER_HZ',window_definition='PERIODIC_HANN_Q1_17',window_sum_squares=sum(int(x,16)**2 for x in []),band_definition_id='HOST_BANDS',bands=[[1000*10**((i-15)/10)/10**0.05,1000*10**((i-15)/10)*10**0.05] for i in range(10)],target_low_hz=31.5,target_high_hz=250,event_thresholds=dict(high_db=6,low_db=3,background_seconds=60,min_active_seconds=5,tail_seconds=20,high_updates='3',low_updates='16',power_floor=5e-10),calibration_id=None)
hann=[int(x,16) for x in (ROOT/'generated/hann4096.mem').read_text().split()]
hann=[x if x<0x20000 else x-0x40000 for x in hann]
assert hashlib.sha256(b''.join(struct.pack('<i',x) for x in hann)).hexdigest()==params['hann']['sha256_le_int32']
config['window_sum_squares']=sum((x/131072)**2 for x in hann)
stages=[]
for k,stage in enumerate(params['stages'],1):
 values=list(map(int,re.search(r'lfn_fir_stage'+str(k)+r'\[\d+\] = \{([^}]+)',header)[1].split(',')))
 assert len(values)==stage['taps'] and hashlib.sha256(b''.join(struct.pack('<i',x) for x in values)).hexdigest()==stage['sha256_le_int32']
 stages.append(dict(decimation=stage['decimation'],phase=stage['phase'],taps=len(values),values=values,sha256=stage['sha256_le_int32']))
config['coefficients']=dict(format='SIGNED_Q1_17',rounding='NEAREST_TIES_AWAY',accumulator_bits=56,stages=stages)
save('config_test',1,7,0,0,0,config)
capture=base(7,9);capture.update(capture_revision='1',state='OPEN',mode=0,source=1,channel=0,pattern=0,origin_raw='0',first_valid_raw='1679',bound_mask=11,first_stream_seq=['5','6',None,'9007199254740993'],first_candidate_raw=['1679','1679',None,'1679'],end_raw=None,start_monotonic_ns='9007199254741000',end_monotonic_ns=None,mono_anchor_raw='1679',mono_anchor_ns='9007199254741000',mono_ns_per_raw=1e9/48000,mono_uncertainty_ns='10000000',end_reason=None)
save('capture_test',12,7,9,0,0,capture)
time=base(7,9);time.update(map_id='1',anchor_raw='1679',valid_from_raw='1679',utc_ns=None,slope_ns_per_raw=1e9/48000,uncertainty_ns='10000000',clock_source='UNSYNCHRONIZED',synchronized=False);save('time_relative',3,7,9,1679,1679,time)
time2=dict(time,map_id='2',anchor_raw='10000',valid_from_raw='10000',utc_ns='-9007199254740993',synchronized=True,clock_source='HOST_FIXTURE');save('time_utc',3,7,9,10000,10000,time2)
config_adc=json.loads(json.dumps(config));config_adc.update(config_id=8,mode=1,source=0);save('config_adc',1,8,0,0,0,config_adc)
cap_adc=json.loads(json.dumps(capture));cap_adc.update(config_id=8,capture_id='10',mode=1,source=0,origin_raw='500',first_valid_raw='500',bound_mask=4,first_stream_seq=[None,None,'7',None],first_candidate_raw=[None,None,'500',None],mono_anchor_raw='500');save('capture_adc',12,8,10,500,500,cap_adc)
closed=dict(capture,capture_revision='2',state='CLOSED',end_raw='300000',end_monotonic_ns='9007199264741000',end_reason='HOST_STOP');save('capture_closed',12,7,9,0,300000,closed)
cal=base(7);cal.update(calibration_id='1',hardware_id='HOST_FIXTURE',gain_setting_id='UNVERIFIED',status='INVALID',invalid_reason='NOT_CALIBRATED',pa_per_fs=None);save('cal_invalid',2,7,0,0,0,cal)
config_cal=json.loads(json.dumps(config));config_cal.update(config_id=9,source=0,gain_status='VERIFIED',analog_gain_db=1.25,gain_setting_id='HOST_GAIN_FIXTURE',calibration_id='2');save('config_cal',1,9,0,0,0,config_cal)
cap_cal=json.loads(json.dumps(capture));cap_cal.update(config_id=9,capture_id='11',source=0);save('capture_cal',12,9,11,0,0,cap_cal)
cal_valid=base(9);cal_valid.update(calibration_id='2',hardware_id='HOST_FIXTURE',gain_setting_id='HOST_GAIN_FIXTURE',status='VALID',reference_id='HOST_CAL_FIXTURE',uncertainty_db=0.5,low_hz=31.5,high_hz=250,pa_per_fs=0.123,date_map_id='2',response_correction=[[31.5,1],[250,1.1]]);save('cal_valid',2,9,0,0,0,cal_valid)
# Unknown optional fields accepted; duplicate keys/escaped aliases/UTF8 errors rejected.
extra=dict(session,optional=dict(note='中文说明',nested=[1,True,None]));(out/'optional.json').write_text(json.dumps(extra,ensure_ascii=False),encoding='utf-8')
# Every required field is removed in turn, with the original binary identity.
bad_manifest=[]
for line in manifest:
 name,typ,cfg,cap,first,end,seq=line.split();body=json.loads((out/(name+'.json')).read_text())
 for key in body:
  changed=dict(body);del changed[key];badname=name+'_missing_'+key
  (out/(badname+'.json')).write_text(json.dumps(changed,separators=(',',':')),encoding='utf-8')
  bad_manifest.append(' '.join([badname,typ,cfg,cap,first,end,seq])+'\n')
(out/'bad_manifest.txt').write_text(''.join(bad_manifest),encoding='utf-8')
extra=[]
for k in range(21):
 body=dict(capture,capture_id=str(20+k));name='extra_capture_'+str(k)
 (out/(name+'.json')).write_text(json.dumps(body,separators=(',',':')),encoding='utf-8')
 extra.append(f'{name} 12 7 {20+k} 0 0 {100+k}\n')
(out/'extra_manifest.txt').write_text(''.join(extra),encoding='utf-8')
manifest_header='#ifndef LFN_RECORD_MANIFEST_H\n#define LFN_RECORD_MANIFEST_H\n#define LFN_METADATA_HANN_SHA "'+params['hann']['sha256_le_int32']+'"\nstatic const char lfn_metadata_stage_sha[3][65] = {'+','.join('"'+x['sha256_le_int32']+'"' for x in params['stages'])+'};\n#endif\n'
manifest_header+=('#define LFN_METADATA_WINDOW_SUM '+repr(config['window_sum_squares'])+'\n#define LFN_METADATA_ALGORITHM_SHA '+json.dumps(config['algorithm_hash'])+'\n#define LFN_METADATA_ROM_SHA '+json.dumps(config['rom_manifest_hash'])+'\n')
(out/'record_manifest.h').write_text(manifest_header,encoding='utf-8')
(out/'manifest.txt').write_text(''.join(manifest),encoding='utf-8')
print('PASS metadata_fixtures CURRENT_FIR_HANN_HASHES_HOST_ONLY')
