/* Shared display model: no generated data, no browser timestamps as sample times. */
(function (root) {
  'use strict';
  function parseReport(data) {
    if (Array.isArray(data?.bands) && data.bands.some(b => b?.spl_db !== undefined)) {
      throw new Error('这是声压级结果文件。要查看采集详情，请导入板端原始报告 JSON。');
    }
    if (data?.schema_version !== 1 || data.kind !== 'noise_report') throw new Error('不支持的报告格式');
    // UART REPORT LIVE predates the network data_source tag.
    const live = data.data_source === 'LIVE_SNAPSHOT' || (data.data_source == null && !!data.live && !data.stored);
    const section = live ? data.live : data.stored;
    if (!section || !Array.isArray(section.bands) || section.bands.length !== 10) throw new Error('报告缺少 10 个频带');
    for (const b of section.bands) {
      if (!Number.isFinite(b.low_hz) || !Number.isFinite(b.high_hz) || b.low_hz <= 0 || b.high_hz <= b.low_hz || (!Number.isFinite(b.dbfs) && !(b.below_floor && b.dbfs === null))) throw new Error('频带数据无效');
    }
    const ns = v => typeof v === 'number' || (typeof v === 'string' && /^\d+$/.test(v)) ? Number(v) : NaN;
    const duration = ns(section.duration_ns ?? data.duration_ns);
    const valid = ns(section.valid_ns ?? data.valid_ns);
    const identity = [data.session_id, data.capture_id, data.config_id].map(String).join('/');
    const boardProgress = data.frame_seq ?? data.sample_raw ?? data.live?.frame_seq ?? data.live?.sample_raw;
    const hostObservation = live && boardProgress == null && /^\d+$/.test(data.host_observation_seq ?? '');
    const progress = boardProgress ?? (hostObservation ? data.host_observation_seq : null);
    return {raw: data, live, session: String(data.session_id), capture: String(data.capture_id), config: data.config_id,
      identity, token: live && progress != null ? String(progress) : null, hostObservation, bands: section.bands,
      duration, valid, coverage: Number.isFinite(duration) && duration > 0 && valid >= 0 && valid <= duration ? valid / duration * 100 : null,
      markers: Array.isArray(data.markers) ? data.markers : []};
  }
  function createHistory() { return {identity: null, token: null, points: []}; }
  function appendHistory(history, report, receivedAt) {
    if (!report.live || report.token === null) return false;
    if (history.identity !== report.identity) {
      history.identity = report.identity; history.token = null; history.points = [];
    }
    if (history.token === report.token) return false;
    if (history.token !== null && /^\d+$/.test(history.token) && /^\d+$/.test(report.token) && BigInt(report.token) < BigInt(history.token)) history.points = [];
    history.token = report.token;
    history.points.push({receivedAt, token: report.token, values: pressureBands(report).map(b => b.spl_db)});
    if (history.points.length > 180) history.points.shift();
    return true;
  }
  function captureLabel(capture) {
    // Values follow capture_application.h and capture_runtime.h, respectively.
    const application = ['WAIT', 'PROBE', 'CLOCK', 'BIND_DMA', 'ACTIVE', 'FAULT', 'BOOT_PROOF'];
    const runtime = ['WAIT', 'PROBE', 'CLOSING', 'COMMIT', 'PLAN', 'ARM', 'START', 'BIND', 'RUN', 'STOPPED', 'FAILED', 'INCOMPATIBLE', 'QUARANTINED'];
    const app = typeof capture.state === 'number' ? application[capture.state] : capture.state?.replace(/^CA_/, '');
    const run = typeof capture.runtime_state === 'number' ? runtime[capture.runtime_state] : capture.runtime_state?.replace(/^CR_/, '');
    if (app === 'FAULT' || (capture.failure != null && capture.failure !== 0)) return '采集故障';
    const labels = {WAIT: '等待采集', PROBE: '检查接口', CLOSING: '停止排空中', COMMIT: '提交配置', PLAN: '准备采集', ARM: '准备数据通道', START: '启动中', BIND: '绑定采集', RUN: '正在采集', STOPPED: '已停止', FAILED: '采集失败', INCOMPATIBLE: '接口不兼容', QUARANTINED: '故障隔离'};
    if (['FAILED', 'INCOMPATIBLE', 'QUARANTINED'].includes(run)) return labels[run];
    if (app === 'ACTIVE') return labels[run] ?? '就绪 · 运行状态未知';
    return {WAIT: '等待启动', PROBE: '检查接口', CLOCK: '配置时钟', BIND_DMA: '绑定数据通道', BOOT_PROOF: '启动安全检查'}[app] ?? labels[run ?? app] ?? '未知';
  }
  function storageLabel(storage) {
    if (storage.failed) return '存储故障';
    if (storage.closed === true || storage.closed === 1) return '已关闭';
    if (storage.active) return '存储已开启';
    if (storage.active === false || storage.active === 0) return '未开启';
    return '未知';
  }
  const centers = [31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250];
  function centerLabel(band) {
    const center = Math.sqrt(band.low_hz * band.high_hz);
    const nominal = centers.find(hz => Math.abs(center / hz - 1) < 0.02);
    return String(nominal ?? Number(center.toFixed(1)));
  }
  function boardLevelText(report) {
    if (!report) return '';
    const bands = pressureBands(report).filter(b => b.spl_db !== null);
    if (!bands.length) return '';
    if (bands.length === 1) return bands[0].center_hz + ' Hz 声压级 ' + bands[0].spl_db + ' dB';
    return '显示板端声压级（dB）。';
  }
  function bandFindings(report) {
    const bands = pressureBands(report).filter(b => b.spl_db !== null);
    if (!bands.length) return null;
    const peak = Math.max(...bands.map(b => b.spl_db));
    return {leaders: bands.filter(b => b.spl_db === peak).map(b => b.center_hz), peak};
  }
  function findingsText(report) {
    const result = bandFindings(report);
    if (!result) return '当前报告没有可用的频带声压级。';
    const tied = result.leaders.length > 1;
    return result.leaders.join('、') + ' Hz 频带声压级' + (tied ? '并列最高' : '最高') + '，为 ' + result.peak + ' dB。';
  }
  function qualityText(report) {
    const scope = report.live ? '当前快照' : '汇总时段';
    if (report.coverage === null) return scope + '覆盖率未知，请核对总时长与有效时长。';
    const missing = (report.duration - report.valid) / 1e9;
    return scope + '有效覆盖 ' + report.coverage.toFixed(1) + '%' +
      (missing > 0 ? '，未覆盖 ' + missing.toFixed(2) + ' s。频带判断仅适用于有记录的时段。' : '。');
  }
  function screeningText(report) {
    if (report.raw.simulation || report.raw.source_mode === 'TEST') return '测试或模拟数据仅供数字链路检查。';
    if (report.raw.source_mode !== 'ADC') return '输入来源未确认，需取得真实 ADC 记录再排查。';
    if (report.valid === 0) return '有效时长为零，请重新采集一段有效记录。';
    if (report.raw.clipped_samples > 0) return '报告记录到输入削顶，请降低增益并重新采集后再比对。';
    const levels = new Map(pressureBands(report).filter(b => b.spl_db !== null).map(b => [b.center_hz,b.spl_db]));
    const peaks = [], assessed = [];
    for (const [lower, center, upper] of [[40,50,63],[80,100,125],[160,200,250]]) {
      if (!levels.has(lower) || !levels.has(center) || !levels.has(upper)) continue;
      assessed.push(center);
      const neighbor = Math.max(levels.get(lower), levels.get(upper));
      if (levels.get(center) >= neighbor + 6) {
        peaks.push(center + ' Hz 比两侧较高者高 ' + (levels.get(center) - neighbor).toFixed(1) + ' dB');
      }
    }
    if (!assessed.length) return '目标频带及其两侧的可测数据不足，无法比较局部突出程度。';
    if (!peaks.length) return '可比较的 ' + assessed.join('、') + ' Hz 频带均未达到比两侧各高至少 6 dB 的筛选条件。可按住户标记的时段，对照频谱与设备工况。';
    return peaks.join('；') + '。这些频带达到本机 6 dB 筛选条件。可在同一测点保持增益不变，分别记录疑似设备运行、停止时的频带变化，再核对住户标记。单凭频带无法识别具体设备。';
  }
  function comparisonRows(report) {
    const c = report.raw.comparison;
    if (c?.status !== 'OK' || !Array.isArray(c.bands) || c.bands.length !== report.bands.length) return [];
    return c.bands.map((b,i) => [centerLabel(report.bands[i]),
      !b.below_floor && Number.isFinite(b.delta_db) ? b.delta_db : null]);
  }
  function comparisonText(report) {
    if (!report?.raw.comparison) return '读取两份 SD 汇总，或导入含对比结果的报告。';
    if (!comparisonRows(report).length) return '两份报告不可比较，请核对测点、采集配置与频带。';
    return '板端对比给出同频带的相对电平变化（dB），负值表示治理后电平降低。请核对测点、增益和工况是否一致。';
  }
  function pressureBands(report) {
    const adc = report.raw.source_mode === 'ADC' && !report.raw.simulation && report.valid !== 0;
    return report.bands.map(band => ({
      center_hz: Number(centerLabel(band)),
      spl_db: adc && !band.below_floor && Number.isFinite(band.estimated_spl_db) ? band.estimated_spl_db : null
    }));
  }
  function reportCSV(report) {
    const rows = pressureBands(report).map(({center_hz,spl_db}) => center_hz + ',' + (spl_db ?? ''));
    return '\uFEFF' + ['center_hz,spl_db', ...rows].join('\r\n') + '\r\n';
  }
  function reportHTML(report) {
    const rows = pressureBands(report).map(({center_hz,spl_db}) =>
      '<tr><td>' + center_hz + '</td><td>' + (spl_db ?? '—') + '</td></tr>').join('');
    return '<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><title>频带声压级</title>' +
      '<style>body{font:16px system-ui;max-width:680px;margin:32px auto;padding:16px}table{width:100%;border-collapse:collapse}th,td{padding:8px;text-align:right;border-bottom:1px solid #ccc}@media print{body{margin:0}tr{break-inside:avoid}}</style></head>' +
      '<body><h1>频带声压级</h1><table><thead><tr><th>中心频率 (Hz)</th><th>声压级 (dB)</th></tr></thead><tbody>' +
      rows + '</tbody></table></body></html>';
  }
  const api = {parseReport, createHistory, appendHistory, captureLabel, storageLabel, centerLabel, boardLevelText, bandFindings, findingsText, qualityText, screeningText, comparisonRows, comparisonText, pressureBands, reportCSV, reportHTML};
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.MonitorModel = api;
})(globalThis);
