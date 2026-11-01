import json
import unittest
from unittest.mock import patch

import serial_board as driver


class Wire:
    def __init__(self):
        self.commands = []
        self.pending = b''
        self.session = '150'
        self.phase = 2
        self.drop_mark = False
        self.responses = {}

    def reset_input_buffer(self):
        self.pending = b''

    def write(self, raw):
        cmd = raw.decode().rstrip('\n')
        self.commands.append(cmd)
        active = int(self.phase == 2)
        if cmd in self.responses:
            self.pending += self.responses[cmd]
        elif cmd == 'STATUS':
            self.pending += f'LFN hb=7 app=4 rt={8 if active else 9} fail=0 rej=0\r\nLFN ST state={self.phase} start=3 stop=0 cap=1 cfg=1 fail=0 raw=0 source=ADC ch=0\r\n'.encode()
        elif cmd == 'SDSTAT':
            self.pending += f'LFN SDSTAT started=1 active={active} closed={1-active} failed=0 session={self.session} state=0 phase=0 error=0 stage=0 steps=0\r\nLFN SDROT rotations=0 epoch=0 gen=0 rest=0 rc=-1 rot=0/0\r\nLFN SDEND files=4\r\n'.encode()
        elif cmd.startswith('REPORT') or cmd.startswith('COMPARE'):
            live = cmd == 'REPORT LIVE'
            section = 'live' if live else 'stored'
            report = {'schema_version':1, 'kind':'noise_report','session_id':self.session,
                      'capture_id':'1','config_id':1,'source_mode':'ADC',section:{'bands':[{'dbfs':-40}]*10}}
            if not live:
                report['data_source'] = 'SD_RECORDS'
            data = (json.dumps(report)+'\n').encode()
            self.pending += ('LFN OK '+cmd.split()[0]+f' bytes={len(data)}'+('' if live else ' source=SD')+'\r\n').encode()+data+b'\r\n'
        elif cmd.startswith('MARK'):
            if self.drop_mark: raise OSError('ack lost')
            self.pending += b'LFN OK MARK id=42 state=QUEUED\r\n'
        elif cmd == 'STOP':
            self.phase = 4
            self.pending += b'LFN OK id=2\r\n'
        elif cmd.startswith('BEGIN '):
            self.session = cmd.split()[1]
            self.pending += b'LFN OK BEGIN\r\n'
        elif cmd.startswith('STARTADC'):
            self.phase = 2
            self.pending += b'LFN OK id=1\r\n'
        else:
            raise AssertionError(cmd)
        return len(raw)

    def read_until(self, expected=b'\n', size=None):
        split = self.pending.index(expected) + len(expected)
        return self.read(split)

    def read(self, n):
        data = self.pending[:n]
        self.pending = self.pending[n:]
        return data

    def close(self): pass


class SerialBoardTests(unittest.TestCase):
    def setUp(self):
        self.wire = Wire()
        patcher = patch('serial_board.open_port',return_value=self.wire)
        self.open = patcher.start()
        self.addCleanup(patcher.stop)
        self.board = driver.SerialBoard('COM19')
        self.addCleanup(self.board.close)

    def call(self, method, path, data=None):
        code, raw = self.board.request(method,path,data)
        return code,json.loads(raw)

    def test_two_uart_reads_advance_only_the_host_observation_counter(self):
        reads = [self.call('GET','/api/spectrum')[1] for _ in range(2)]
        self.assertLess(int(reads[0]['host_observation_seq']),int(reads[1]['host_observation_seq']))
        self.assertEqual(reads[0]['data_source'],'LIVE_SNAPSHOT')
        self.assertNotIn('frame_seq', reads[0])
        self.assertNotIn('sample_raw', reads[0])
        self.assertEqual(reads[0]['live'],reads[1]['live'])
        self.assertEqual(self.wire.commands.count('REPORT LIVE'),2)

    def test_active_session_adoption_and_repeat_start_stop(self):
        self.assertEqual(self.call('GET','/api/capture')[1]['phase'],'running')
        self.assertEqual(self.call('POST','/api/capture/start/151')[0],409)
        self.assertNotIn('BEGIN 151',self.wire.commands)
        self.assertEqual(self.call('POST','/api/capture/stop')[1]['phase'],'stopped')
        self.assertTrue(self.call('POST','/api/capture/stop')[1]['storage']['closed'])
        self.assertEqual(self.wire.commands.count('STOP'),1)
        self.assertEqual(self.call('POST','/api/capture/start/151')[1]['phase'],'running')
        self.assertEqual(self.wire.commands.count('BEGIN 151'),1)

    def test_repeated_marker_and_lost_ack_do_not_write_twice(self):
        cmd = {'request_id':'1','strength':3,'note':'窗边'}
        self.assertEqual(self.call('POST','/api/marker',cmd)[1]['marker_id'],'42')
        self.assertEqual(self.call('POST','/api/marker',cmd)[1]['marker_id'],'42')
        self.wire.drop_mark = True
        cmd['request_id'] = '2'
        self.assertGreaterEqual(self.call('POST','/api/marker',cmd)[0],500)
        self.assertGreaterEqual(self.call('GET','/api/marker/2')[0],500)
        self.assertGreaterEqual(self.call('POST','/api/marker',cmd)[0],500)
        self.assertEqual(self.wire.commands.count('MARK 3 窗边'),2)

    def test_sd_paths_and_command_injection(self):
        self.wire.phase = 4
        payload = {'request_id':'1','path':'SR000001/ASUM.LFR','compare_path':'SR000002/E000001/BSUM.LFR'}
        self.assertEqual(self.call('POST','/api/report',payload)[1]['data_source'],'SD_RECORDS')
        self.assertIn('COMPARE SR000001/ASUM.LFR SR000002/E000001/BSUM.LFR',self.wire.commands)
        payload.update(request_id='2',path='x\nSTOP')
        self.assertEqual(self.call('POST','/api/report',payload)[0],400)
        self.assertNotIn('STOP',self.wire.commands)

    def test_close_is_terminal(self):
        self.board.close()
        self.assertGreaterEqual(self.call('GET','/api/capture')[0],500)
        self.open.assert_not_called()
