import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from session_recording import SessionRecording
from server import Archive
def frame(seq, session='7'):
    centers = [31.5,40,50,63,80,100,125,160,200,250]
    return dict(schema_version=1,kind='noise_report',source_mode='ADC',data_source='LIVE_SNAPSHOT',
                session_id=session,capture_id='3',config_id=1,frame_seq=str(seq),
                live={'bands':[dict(low_hz=c*.9,high_hz=c/.9,dbfs=-50,estimated_spl_db=71+i,
                                    below_floor=False) for i,c in enumerate(centers)]})


class SessionRecordingTests(unittest.TestCase):
    def test_persist_unbounded_history_with_native_and_serial_progress(self):
        with TemporaryDirectory() as tmp:
            recorder = SessionRecording(Path(tmp))
            for index in range(251):
                data = frame(index)
                recorder.point(Archive._frame(data), 10000+index, serial=False)
            recorder.point(Archive._frame(frame(250)), 11000, serial=False)
            scrambled=frame(1)
            scrambled['live']['bands'].reverse()
            recorder.point(Archive._frame(scrambled), 12000, serial=True)
            recorder.close()
            recorder = SessionRecording(Path(tmp))
            try:
                d = recorder.detail('7/3/1')
                self.assertEqual(d['count'],252)
                self.assertEqual(d['points'][-1], {'time_ms':12000,'token':'1','values':list(range(71,81))})
                with self.assertRaises(KeyError):
                    recorder.detail('8/3/1')
            finally:
                recorder.close()

    def test_markers_retry_and_summary_use_one_segment_and_original_host_time(self):
        with TemporaryDirectory() as tmp:
            r=SessionRecording(Path(tmp))
            try:
                r.point(Archive._frame(frame(1)), 10000, serial=False)
                r.marker_request('7/3/1','842',4,'住宅',10100)
                r.marker_request('7/3/1','842',4,'住宅',13000)
                r.marker_reply('7/3/1','842','1')
                sd=frame(1)
                sd.update(data_source='SD_RECORDS',stored=sd.pop('live'),
                          markers=[{'marker_id':1,'strength':4,'note':'住宅','start_raw':2**63},
                                   {'marker_id':2,'strength':1,'note':'另一终端','start_raw':44}])
                r.summary(sd)
                d=r.detail('7/3/1')
                self.assertEqual(d['markers'][0]['time_ms'],10100)
                self.assertEqual(d['markers'][0]['point_time_ms'],10000)
                self.assertEqual(d['markers'][0]['status'],'synced')
                self.assertEqual(d['markers'][0]['start_raw'],str(2**63))
                self.assertIsNone(d['markers'][1]['time_ms'])
                self.assertEqual(d['markers'][1]['values'],[None]*10)
                self.assertTrue(r.detail(None,metadata=True)['summary'])
                self.assertEqual(r.detail('7/3/1')['summary']['session_id'],'7')
            finally:
                r.close()

class SessionHTTPTests(unittest.TestCase):
    def test_background_observations_marker_retry_and_stopped_bundle(self):
        import threading, time
        from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
        from urllib.request import Request,urlopen
        from urllib.error import HTTPError
        import server
        class Board(BaseHTTPRequestHandler):
            phase='running'
            seq=0
            def log_message(self,*args): pass
            def send(self,code,data):
                body=json.dumps(data).encode()
                self.send_response(code);self.send_header('Content-Length',str(len(body)));self.end_headers()
                self.wfile.write(body)
            def do_GET(self):
                if self.path=='/api/capture':
                    active=type(self).phase=='running'
                    self.send(200,dict(phase=type(self).phase,session='7',capture_id='3',config_id=1,source='ADC',
                              channel=0,failure=0,may_be_running=active,
                              storage=dict(available=True,active=active,failed=False,closed=not active)))
                elif self.path=='/api/spectrum':
                    type(self).seq+=1
                    self.send(200,frame(type(self).seq))
                elif self.path=='/api/marker/42':
                    self.send(200,dict(state='QUEUED',request_id='42',marker_id='9'))
                else:
                    self.send(404,{})
            def do_POST(self):
                self.rfile.read(int(self.headers.get('Content-Length','0')))
                if self.path=='/api/marker':
                    self.send(202,dict(request_id='42',state='PENDING'))
                elif self.path=='/api/report':
                    data=frame(1)
                    data.update(data_source='SD_RECORDS',stored=data.pop('live'),
                                markers=[dict(marker_id='9',note='听到嗡声',strength=4,start_raw=1000)])
                    self.send(200,data)
                else:self.send(404,{})
        board=ThreadingHTTPServer(('127.0.0.1',0),Board)
        threading.Thread(target=board.serve_forever,daemon=True).start()
        try:
            with TemporaryDirectory() as temp:
                app=server.make_server('127.0.0.1',0,f'http://127.0.0.1:{board.server_port}',export_dir=Path(temp)/'Reports')
                threading.Thread(target=app.serve_forever,daemon=True).start()
                url=f'http://127.0.0.1:{app.server_port}'
                def request(path,payload=None,host=None):
                    headers={'Content-Type':'application/json'}
                    if host: headers['Host']=host
                    req=Request(url+path,None if payload is None else json.dumps(payload).encode(),headers)
                    try:resp=urlopen(req,timeout=6)
                    except HTTPError as err:resp=err
                    with resp:
                        raw=resp.read()
                        return resp.code,json.loads(raw) if 'application/json' in resp.headers['Content-Type'] else raw
                try:
                    ids=dict(session='7',capture='3',config='1')
                    self.assertEqual(request('/api/session-detail')[0],404)
                    self.assertEqual(request('/api/session-detail',host='evil.example')[0],403)
                    self.assertEqual(request('/api/session-preview',host='evil.example')[0],403)
                    self.assertEqual(request('/api/capture')[1]['phase'],'running')
                    # No page spectrum requests at all; the local service must continue polling.
                    until=time.monotonic()+8
                    while time.monotonic()<until:
                        status,meta=request('/api/session-detail?metadata=1')
                        if status==200 and meta['count']>=2:break
                        time.sleep(.15)
                    self.assertGreaterEqual(meta['count'],2)
                    self.assertIn('接入时板卡已在采集',meta['warning'])
                    self.assertEqual(request('/api/export-session',ids)[0],409)
                    self.assertEqual(request('/api/marker',dict(request_id='42',strength=4,note='听到嗡声'))[0],202)
                    self.assertEqual(request('/api/marker/42')[0],200)
                    Board.phase='stopped'
                    self.assertEqual(request('/api/capture')[1]['phase'],'stopped')
                    old=request('/api/session-detail?metadata=1')[1]['count']
                    request('/api/spectrum')  # A stopped board can still publish its last FFT.
                    self.assertEqual(request('/api/session-detail?metadata=1')[1]['count'],old)
                    self.assertEqual(request('/api/report',dict(request_id='43',path='',compare_path=''))[0],200)
                    status,d=request('/api/session-detail?session=7&capture=3&config=1')
                    self.assertEqual(status,200)
                    self.assertEqual(d['markers'][0]['status'],'synced')
                    self.assertEqual(d['markers'][0]['values'],list(range(71,81)))
                    self.assertEqual(request('/api/session-preview')[0],200)
                    code,receipt=request('/api/export-session',ids)
                    self.assertEqual(code,201,receipt)
                    self.assertEqual(Path(receipt['path']).stat().st_size,receipt['bytes'])
                finally:
                    app.shutdown();app.server_close()
        finally:
            board.shutdown();board.server_close()
