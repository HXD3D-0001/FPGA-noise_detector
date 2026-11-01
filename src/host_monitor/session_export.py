"""Offline, pressure-only reports from the host's full observation journal."""
import bisect
import csv
from datetime import datetime, timezone
from html import escape
import io
import math
import os
from pathlib import Path
import re
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

MARK_WINDOW_MS = 5000
GAP_MS = 8000
PLOT_WIDTH = 790


def align_markers(points, markers):
    """Associate host submission times with the nearest host reception in the same capture."""
    ordered = sorted(points, key=lambda p: p['time_ms'])
    times = [p['time_ms'] for p in ordered]
    result = []
    for marker in markers:
        stamp = marker.get('time_ms')
        nearest = None
        if stamp is not None and times:
            pos = bisect.bisect_left(times, stamp)
            candidates = ordered[max(0, pos-1):pos+1]
            nearest = min(candidates, key=lambda p: (abs(p['time_ms']-stamp), p['time_ms']))
            if abs(nearest['time_ms']-stamp) > MARK_WINDOW_MS:
                nearest = None
        result.append(dict(marker, point_time_ms=nearest['time_ms'] if nearest else None,
                           delta_ms=nearest['time_ms']-stamp if nearest else None,
                           values=nearest['values'] if nearest else [None]*10))
    return result


def utc(stamp):
    if stamp is None:
        return ''
    return datetime.fromtimestamp(stamp/1000, timezone.utc).isoformat(timespec='milliseconds').replace('+00:00','Z')


def session_start(detail):
    times = (p['time_ms'] for group in ('points','markers') for p in detail[group] if p.get('time_ms') is not None)
    return min(times, default=0)


def finite(value):
    return type(value) in (int,float) and math.isfinite(value)


def cell(value):
    return '—' if value is None or value == '' else escape(str(value))


def table(headers, rows):
    head = ''.join('<th>'+cell(x)+'</th>' for x in headers)
    body = ''.join('<tr>'+''.join('<td>'+cell(x)+'</td>' for x in row)+'</tr>' for row in rows)
    return '<table><thead><tr>'+head+'</tr></thead><tbody>'+body+'</tbody></table>'


def marker_fields(marker):
    state = {'synced':'SD 已确认', 'queued':'已提交', 'unconfirmed':'应答未确认'}.get(marker.get('status'),'未知')
    return [marker.get('marker_id'), state, utc(marker.get('time_ms')), marker.get('strength'), marker.get('note'),
            marker.get('start_raw'), utc(marker.get('point_time_ms')),
            marker['delta_ms']/1000 if marker.get('delta_ms') is not None else None, *marker['values']]


def marker_headers(detail):
    return ['标记编号','保存状态','提交时间（UTC）','强度（1–5）','住户备注','板端样本位置','对应读数时间（UTC）','时间差（s）',
            *[f'{hz} Hz（dB）' for hz in detail['centers']]]


def excel_cell(value):
    if isinstance(value, str) and (value.startswith(('\t','\r','\n')) or value.lstrip().startswith(('=','+','-','@'))):
        return "'"+value
    return value


def write_csv(bundle, name, headers, rows):
    # Stream the original point rows directly; the chart's display sampling does not affect CSV.
    with bundle.open(name, 'w', force_zip64=True) as raw:
        with io.TextIOWrapper(raw, encoding='utf-8-sig', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(headers)
            for row in rows:
                writer.writerow([excel_cell(v) for v in row])


def render_chart(detail, index, start, end):
    hz = detail['centers'][index]
    points = detail['points']
    left, top, bottom = 56, 34, 159
    xmax = left+PLOT_WIDTH
    x = lambda t: left + (t-start)/max(1,end-start)*PLOT_WIDTH
    lo, hi = math.inf, -math.inf
    for point in points:
        value = point['values'][index]
        if finite(value):
            lo, hi = min(value,lo), max(value,hi)
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 880 200" role="img" aria-label="{hz} Hz 声压级时间曲线">',
           '<rect width="880" height="200" fill="white"/>',
           f'<g font-family="sans-serif" font-size="12" fill="#254655"><text x="8" y="19">{hz} Hz · 声压级（dB）</text>']
    if not math.isfinite(lo):
        svg.append('<text x="110" y="98">该频带无可用声压级读数</text></g></svg>')
        return ''.join(svg)
    low = math.floor(lo/5)*5
    high = max(low+5,math.ceil(hi/5)*5)
    y = lambda v: bottom - (v-low)/(high-low)*(bottom-top)
    for i in range(5):
        value = low+(high-low)*i/4
        yy = y(value)
        svg.append(f'<path d="M{left} {yy:.2f}H{xmax}" stroke="#dce6e8"/><text x="{left-7}" y="{yy+4:.2f}" text-anchor="end">{value:g}</text>')
    span = end-start
    for i in range(5 if span else 1):
        xx = left+i*PLOT_WIDTH/4
        svg.append(f'<text x="{xx:.1f}" y="178" text-anchor="middle">{span*i/4000:.1f}</text>')
    svg.append('<text x="849" y="195" text-anchor="end">主机时间（s）</text>')

    # Keep first, extrema and last points per horizontal pixel. Missing data and pauses split paths.
    commands, bucket, last_t = [], [], None
    pixel = None
    segment_len = 0
    def flush_bucket():
        nonlocal segment_len
        if not bucket:
            return
        keep = sorted({0,len(bucket)-1,min(range(len(bucket)),key=lambda i:bucket[i][1]),
                       max(range(len(bucket)),key=lambda i:bucket[i][1])})
        for i in keep:
            t,v = bucket[i]
            commands.append(f'{"M" if segment_len == 0 else "L"}{x(t):.2f} {y(v):.2f}')
            segment_len += 1
        bucket.clear()

    def end_segment():
        nonlocal segment_len
        flush_bucket()
        if segment_len == 1:  # round line caps render an isolated point
            commands.append('l0 0')
        segment_len = 0

    for p in points:
        t, v = p['time_ms'], p['values'][index]
        if not finite(v):
            end_segment()
            last_t = None
            continue
        if last_t is not None and (t < last_t or t-last_t > GAP_MS):
            end_segment()
        col = int(x(t))
        if col != pixel:
            flush_bucket()
        pixel, last_t = col, t
        bucket.append((t,v))
    end_segment()
    svg.append('<path d="'+' '.join(commands)+'" fill="none" stroke="#087b76" stroke-width="2.1" stroke-linecap="round"/>')
    for m in detail['markers']:
        if m.get('time_ms') is None:
            continue
        xx=x(m['time_ms'])
        label=escape(str(m.get('marker_id') or '?'))
        svg.append(f'<g><title>标记 #{label} · {utc(m["time_ms"])}</title><path d="M{xx:.2f} {top}V{bottom}" stroke="#b56128" stroke-dasharray="3 4"/>'
                   f'<text x="{min(xmax-20,xx+2):.2f}" y="30" fill="#874317">#{label}</text></g>')
    svg.append('</g></svg>')
    return ''.join(svg)


def summary_levels(detail):
    data = detail.get('summary') or {}
    stored = data.get('stored') or {}
    values = {}
    if data.get('source_mode')=='ADC' and not data.get('simulation') and stored.get('valid_ns', data.get('valid_ns')) not in (0,'0'):
        for b in stored.get('bands',[]):
            lo,hi = b.get('low_hz'),b.get('high_hz')
            if not b.get('below_floor') and finite(lo) and finite(hi) and 0<lo<hi and finite(b.get('estimated_spl_db')):
                center = math.sqrt(lo*hi)
                for hz in detail['centers']:
                    if abs(center/hz-1)<.02:
                        values[hz]=b['estimated_spl_db']
                        break
    return values


def _charts(detail):
    start = session_start(detail)
    end = max((p['time_ms'] for group in ('points','markers') for p in detail[group] if p.get('time_ms') is not None),default=start)
    return [render_chart(detail,i,start,end) for i in range(len(detail['centers']))]


def render_session_html(detail, charts=None):
    points, markers = detail['points'], detail['markers']
    start = session_start(detail)
    if charts is None:
        charts = _charts(detail)
    heading = f'会话 {cell(detail["session"])}　·　采集段 {cell(detail["capture"])}　·　配置 {cell(detail["config"])}'
    metrics = f'主机起点 {cell(utc(start) if start else "")}　·　{len(points)} 次读数　·　{len(markers)} 条标记'
    warning = '<p role="alert"><strong>记录说明</strong>　'+cell(detail['warning'])+'</p>' if detail.get('warning') else ''
    summary = detail.get('summary')
    if summary:
        stored = summary.get('stored') or {}
        total = stored.get('duration_ns',summary.get('duration_ns'))
        valid = stored.get('valid_ns',summary.get('valid_ns'))
        if finite(total) and finite(valid) and total>0 and 0<=valid<=total:
            metrics += f'　·　板端有效 {valid/1e9:.3f} / {total/1e9:.3f} s（{valid/total:.1%}）'
        levels = summary_levels(detail)
        summary_html = table(['中心频率（Hz）','声压级（dB）'], [[hz,levels.get(hz)] for hz in detail['centers']])
    else:
        summary_html = '<p>SD 汇总尚未取得。</p>'
    marker_section = table(marker_headers(detail),(marker_fields(m) for m in markers)) if markers else '<p>本次采集无住户标记。</p>'
    return ('<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
            '<title>全程频带声压报告</title><style>*{box-sizing:border-box}body{font:15px/1.6 system-ui,"Microsoft YaHei",sans-serif;color:#193c4a;max-width:1140px;margin:auto;padding:20px}'
            'h1,h2{line-height:1.3}svg{display:block;width:100%;height:auto}figure{margin:14px 0;break-inside:avoid;border:1px solid #dae4e6}.table-wrap{overflow:auto}'
            'table{border-collapse:collapse;width:100%;font-size:13px}th,td{padding:5px 8px;border-bottom:1px solid #d9e5e7;text-align:left;overflow-wrap:anywhere}th{background:#edf4f4}'
            '@media print{body{padding:0;font-size:10pt}th,td{padding:2px;font-size:7pt}.table-wrap{overflow:visible}thead{display:table-header-group}tr{break-inside:avoid}}</style></head>'
            '<body><h1>全程频带声压报告</h1><p>'+heading+'</p><p>'+metrics+'</p>'+warning+'<h2>频带声压级汇总</h2>'+summary_html+
            '<h2>逐频带时间曲线</h2><p>横轴为从主机起点经过的秒数，纵轴为各频带声压级。橙色虚线标示住户标记的提交时刻。缺值或相邻读数间隔超过 8 秒时断线。'
            '密集读数按横向像素保留首末点和极值，完整时间序列 CSV 包含全部记录。</p>'+
            ''.join('<figure>'+chart+'</figure>' for chart in charts)+'<h2>住户标记与频带声压级</h2>'
            '<p>取同一采集段中距离主机提交时刻最近的一次读数，允许前后 5 秒，等距时取较早读数。时间差为读数时间减提交时间。'
            '缺少提交时间或 5 秒内无读数时，对应声压留空。记录时间采用 UTC。</p><div class="table-wrap">'+marker_section+'</div></body></html>')


def write_session_bundle(directory, detail):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True,exist_ok=True)
    sid = re.sub('[^a-zA-Z0-9_-]','_',str(detail['session']))[:40]
    descriptor, name = tempfile.mkstemp(dir=directory,prefix='noise-session-'+sid+'-',suffix='.tmp')
    os.close(descriptor)
    partial = Path(name)
    result = partial.with_suffix('.zip')
    start = session_start(detail)
    try:
        with ZipFile(partial,'w',compression=ZIP_DEFLATED) as bundle:
            headers = ['接收时间（UTC）','经过时间（s）','源帧或主机观测号',*[f'{hz} Hz（dB）' for hz in detail['centers']]]
            write_csv(bundle,'完整时间序列.csv',headers,
                      ([utc(p['time_ms']), (p['time_ms']-start)/1000, p.get('token'), *p['values']] for p in detail['points']))
            write_csv(bundle,'住户标记声压.csv',marker_headers(detail),(marker_fields(m) for m in detail['markers']))
            charts = _charts(detail)
            for hz, svg in zip(detail['centers'], charts):
                bundle.writestr(f'频带曲线/{hz}Hz.svg',svg)
            bundle.writestr('汇总报告.html',render_session_html(detail,charts))
        partial.rename(result)
    finally:
        partial.unlink(missing_ok=True)
    return {'path':str(result),'dir':str(directory),'bytes':result.stat().st_size}
