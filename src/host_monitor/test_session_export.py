import csv
import io
import tempfile
import unittest
from pathlib import Path
from zipfile import ZipFile
from xml.etree.ElementTree import fromstring

from session_export import align_markers, render_session_html, write_session_bundle


class SessionExportTests(unittest.TestCase):
    def test_marker_alignment_bounds_ties_missing_time_and_order(self):
        p = [{'time_ms': 12000, 'values':[40]*10}, {'time_ms':2000, 'values':[30]*10}]
        rows = align_markers(p, [{'time_ms':t} for t in (7000, 17000, None, 17001)])
        self.assertEqual([r['point_time_ms'] for r in rows], [2000,12000,None,None])
        self.assertEqual([r['delta_ms'] for r in rows], [-5000,-5000,None,None])
        self.assertEqual(rows[0]['values'], [30]*10)
        self.assertEqual(rows[-1]['values'], [None]*10)

    def test_csv_and_standalone_report_preserve_full_data_and_escape_text(self):
        centers = [31.5,40,50,63,80,100,125,160,200,250]
        points = [{'time_ms':1700000000000 + i*2500,'token':str(i+1),'values':[None,*(float(i+b) for b in range(9))]} for i in range(301)]
        # One break due to loss, another due to a missing band.
        points[-1]['time_ms'] += 10000
        points[5]['values'][1] = None
        markers = align_markers(points, [{'marker_id':'1', 'strength':4,'note':' =1+1,<&"', 'time_ms': points[0]['time_ms']+25, 'start_raw':'120'},
                                         {'marker_id':'2', 'strength':1,'note':'外部标记','time_ms':None, 'start_raw':'140'}])
        detail = {'session':'123','capture':'1','config':'2','centers':centers,'points':points,'markers':markers,
                  'summary':None,'count':len(points)}
        with tempfile.TemporaryDirectory() as temp:
            ret = write_session_bundle(Path(temp),detail)
            self.assertEqual(Path(ret['path']).stat().st_size,ret['bytes'])
            with ZipFile(ret['path']) as z:
                self.assertIsNone(z.testzip())
                self.assertEqual(len(z.namelist()),13)
                rows = list(csv.reader(io.StringIO(z.read('完整时间序列.csv').decode('utf-8-sig'))))
                self.assertEqual(len(rows),302)
                self.assertEqual(rows[-1][-10:],['',*map(str,points[-1]['values'][1:])])
                rows=list(csv.reader(io.StringIO(z.read('住户标记声压.csv').decode('utf-8-sig'))))
                self.assertIn("' =1+1,<&\"",rows[1])
                self.assertEqual(rows[2][-10:],['']*10)
                self.assertIn('-0.025', rows[1])
                report=z.read('汇总报告.html').decode()
                self.assertEqual(report,render_session_html(detail))
                self.assertEqual(report.count('<svg '),10)
                self.assertIn('SD 汇总尚未取得',report)
                self.assertIn('&lt;&amp;',report)
                svg=z.read('频带曲线/40Hz.svg').decode()
                fromstring(svg)
                self.assertGreaterEqual(svg.count('M'),3)
                self.assertIn('#1',svg)
                self.assertNotRegex(report,r'dBFS|估算|实测')

    def test_summary_uses_physical_band_limits_not_array_index(self):
        detail={'session':'7','capture':'1','config':'1','points':[],'markers':[],'centers':[31.5,40,50,63,80,100,125,160,200,250],
                'summary':{'source_mode':'ADC','stored':{'bands':[{'low_hz':89.125,'high_hz':112.202,'below_floor':False,'estimated_spl_db':74.6}]}}}
        html=render_session_html(detail)
        self.assertRegex(html,r'<td>100</td><td>74.6</td>')
        self.assertNotRegex(html,r'<td>31.5</td><td>74.6</td>')
