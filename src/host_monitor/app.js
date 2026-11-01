'use strict';
const $ = id => document.getElementById(id);
const palette = ['#147d78','#3b8cad','#6b74bf','#ad68a4','#ca7a51','#8b9b3a','#2b9b86','#5b82be','#b68137','#895b91'];
let offline = false, connectionMode = 'offline', generation = 0, report = null, status = null, lastReceived = 0, lastAdvanced = 0;
let lastReportAt = 0, reportStale = false, exportBusy = false, archiveRestored = false;
let connectionBoard = null;
let history = MonitorModel.createHistory();
let pollTimer = null, activePoll = null, activeSelftest = null, selftestBusy = false, configuring = false;
let discoveryBusy = false, linkUnavailable = false;

function serialSelected() { return $('transport').value === 'serial'; }
function showTransport() {
  $('network-field').hidden = serialSelected();
  $('serial-field').hidden = $('ports-refresh').hidden = !serialSelected();
  $('serial-release').hidden = !serialSelected() || connectionMode !== 'serial';
}
let portsRequest = 0;
async function refreshPorts() {
  const request = ++portsRequest;
  try {
    const data = await getJSON('/api/ports');
    if (request !== portsRequest || !serialSelected()) return;
    const select = $('serial-port');
    const previous = select.value || (connectionMode === 'serial' ? connectionBoard : '');
    const ports = data.ports || [];
    select.replaceChildren();
    for (const port of ports.length ? ports : ['']) {
      const option = document.createElement('option');
      option.value = port; option.textContent = port || '未发现串口';
      select.append(option);
    }
    select.value = ports.includes(previous) ? previous : ports[0] || '';
    if (typeof renderCaptureControls === 'function') renderCaptureControls();
  } catch (error) {
    if (request === portsRequest && serialSelected()) showError('无法读取本机串口：' + error.message);
  }
}
async function discoverBoard() {
  if (serialSelected() || offline || configuring || selftestBusy || discoveryBusy || productOperationBusy ||
      pendingProduct.marker || pendingProduct.report) return;
  const run = generation;
  discoveryBusy = true;
  try {
    const result = await getJSON('/api/discovery', {
      method:'POST', headers:{'Content-Type':'application/json'}, body:'{}'
    }, 10000);
    if (run !== generation || serialSelected() || offline || productOperationBusy) return;
    if (result.state === 'found' && result.board) {
      if (document.activeElement === $('board-address')) return;
      linkUnavailable = false;
      await connect({mode:'board', board:result.board});
    } else if (result.state === 'multiple' || result.state === 'select') {
      $('board').textContent = '请选择板卡地址并连接：' + (result.boards || []).join('、');
    } else if (!connectionBoard) {
      $('board').textContent = '正在等待板卡。请接好 ETH0 网线，也可手动填写地址。';
    }
  } catch (error) {
    if (run === generation && !connectionBoard) $('board').textContent = '自动连接暂不可用，可手动填写板卡地址。';
  } finally { discoveryBusy = false; }
}
function badge(text, style) { $('connection').textContent = text; $('connection').className = 'badge ' + style; }
function showError(message) { $('error').textContent = message; $('error').hidden = !message; }
function value(text, fallback = '未知') { return text == null ? fallback : String(text); }
function simulation() { return report?.raw.simulation === true || status?.simulation === true; }
function modeLabel() {
  const mode = offline ? report?.raw.source_mode : report?.raw.source_mode ?? status?.capture?.source_mode ?? status?.capture?.mode;
  $('mode').textContent = '源模式 ' + value(mode);
  $('notice').className = 'notice' + (simulation() ? ' simulation' : mode === 'TEST' ? ' test' : '');
  const level = MonitorModel.boardLevelText(report);
  $('notice').textContent = (simulation() ? '模拟数据 · ' : mode === 'TEST' ? 'TEST 数字测试源 · ' : mode === 'ADC' ? 'ADC 输入 · ' : '源模式未确认 · ') + (level || '暂无声压级读数。');
  $('trend-description').textContent = (report?.hostObservation || connectionMode === 'serial'
    ? '串口每次成功读取一份实时频谱记一个观测点，相邻读数可能来自同一板端帧。'
    : simulation() ? '展示模拟快照的变化。' : '按板端快照进度记录。') + '横轴为电脑接收时间。';
}
function renderFindings(next) {
  if (!next) $('band-axis').textContent = '横轴为中心频率，柱顶为频带声压级（dB）。';
  $('analysis-text').textContent = next ? MonitorModel.findingsText(next) : '等待有效频带数据。';
  $('analysis-quality').textContent = next ? MonitorModel.qualityText(next) : '';
  $('analysis-clue').textContent = next ? MonitorModel.screeningText(next) : '等待采集或导入 ADC 报告。';
  $('comparison-text').textContent = MonitorModel.comparisonText(next);
  const box = $('comparison-table');
  box.replaceChildren();
  const rows = next ? MonitorModel.comparisonRows(next) : [];
  if (!rows.length) return;
  const table = document.createElement('table');
  function row(tag, values) {
    const tr = document.createElement('tr');
    for (const value of values) {
      const cell = document.createElement(tag);
      cell.textContent = value;
      tr.append(cell);
    }
    table.append(tr);
  }
  row('th', ['中心频率（Hz）', '治理后相对治理前的变化（dB）']);
  for (const [hz, delta] of rows) {
    row('td', [hz, delta === null ? '—' : delta.toFixed(2)]);
  }
  box.append(table);
}
function chartLimits(values) {
  const valid = values.filter(Number.isFinite);
  if (!valid.length) return null;
  const high = Math.ceil(Math.max(...valid) / 10) * 10;
  return [Math.min(high - 30, Math.floor(Math.min(...valid) / 10) * 10), high];
}
function renderReport(next) {
  report = next;
  lastReportAt = Date.now(); reportStale = false;
  setExportEnabled(true);
  const origin = next.live ? (offline ? '导入的实时快照 · 非实时' : '板卡实时快照') :
    (offline ? '导入的区间汇总' : '板卡已存报告 · 非实时');
  $('source').textContent = simulation() ? origin + ' · 模拟数据' : origin;
  $('identity').textContent = `Session ${next.session} / Capture ${next.capture} / Config ${next.config}`;
  $('coverage').textContent = next.coverage === null ? '—' : next.coverage.toFixed(1) + '%';
  $('duration').textContent = Number.isFinite(next.duration) && Number.isFinite(next.valid) ? `有效 ${(next.valid / 1e9).toFixed(2)} / 总计 ${(next.duration / 1e9).toFixed(2)} s` : '无时长数据';
  $('band-description').textContent = (!next.live ? '已保存区间' : offline ? '文件中单个采样窗口' : '当前快照') + '的频带声压级';
  $('bands').replaceChildren();
  const levels = MonitorModel.pressureBands(next);
  const range = chartLimits(levels.map(b => b.spl_db));
  $('band-axis').textContent = range ? '横轴为中心频率（Hz），纵轴 ' + range[0] + ' 至 ' + range[1] + ' dB，随数据调整。' : '当前报告没有可显示的频带声压级。';
  levels.forEach(({center_hz, spl_db}, i) => {
    const label = center_hz + ' Hz';
    const item = document.createElement('div'); item.className = 'band';
    const reading = document.createElement('strong'); reading.textContent = spl_db === null ? '—' : spl_db.toFixed(1);
    const meter = document.createElement('div'); meter.className = 'meter';
    const fill = document.createElement('div'); fill.className = 'fill'; fill.style.background = palette[i];
    fill.style.height = (spl_db === null ? 0 : (spl_db - range[0]) / (range[1] - range[0]) * 100) + '%';
    meter.append(fill);
    const name = document.createElement('span'); name.textContent = label;
    item.title = label + ' · ' + (spl_db === null ? '无声压级读数' : reading.textContent + ' dB');
    item.append(reading, meter, name); $('bands').append(item);
  });
  $('marker-count').textContent = next.markers.length + ' 条'; $('markers').replaceChildren();
  if (!next.markers.length) { const empty = document.createElement('div'); empty.className = 'empty'; empty.textContent = '此报告暂无标记'; $('markers').append(empty); }
  next.markers.forEach(marker => {
    const row = document.createElement('div'); row.className = 'marker';
    const id = document.createElement('div'); id.textContent = '#' + value(marker.marker_id, '—');
    const level = document.createElement('div'); level.className = 'marker-level'; level.textContent = '强度 ' + value(marker.strength, '—') + ' / 5';
    const raw = document.createElement('small'); raw.textContent = 'raw ' + value(marker.start_raw, '—') + (marker.end_raw != null ? ' → ' + marker.end_raw : ' · 点标记'); level.append(raw);
    const note = document.createElement('div'); note.className = 'marker-note'; note.textContent = value(marker.note, '无备注');
    row.append(id, level, note); $('markers').append(row);
  });
  renderFindings(next); modeLabel(); drawTrend();
}
function renderStatus(data) {
  status = data;
  const capture = data.capture ?? {}, storage = data.storage ?? {};
  const label = MonitorModel.captureLabel(capture);
  const closed = typeof captureState !== 'undefined' && captureState.confirmed && captureState.phase === 'stopped' &&
    captureState.board === connectionBoard && captureState.session === storage.session &&
    storage.closed === true && storage.active === false && !storage.failed;
  $('capture-state').textContent = closed && label === '正在采集' ? '已停止' : label;
  $('storage-state').textContent = MonitorModel.storageLabel(storage);
  $('storage-detail').textContent = storage.markers ? `标记：已同步 ${value(storage.markers.synced, '—')} · 待同步 ${value(storage.markers.pending, '—')}` : '存储状态快照';
  modeLabel();
}
function drawTrend() {
  const canvas = $('trend'), width = canvas.clientWidth || 900, height = 210, ratio = window.devicePixelRatio || 1;
  $('trend-controls').hidden = offline;
  canvas.width = width * ratio; canvas.height = height * ratio;
  const ctx = canvas.getContext('2d'); ctx.scale(ratio, ratio);
  const left = 42, right = width - 18, top = 10, bottom = 184;
  ctx.font = '10px system-ui'; ctx.fillStyle = '#8390a2'; ctx.strokeStyle = '#e8eef2'; ctx.lineWidth = 1;
  const points = history.points;
  const selected = $('trend-band').value || '2';
  const bands = selected === 'all' ? Array.from({length:10}, (_,i) => i) : [Number(selected)];
  const range = chartLimits(points.flatMap(p => bands.map(i => p.values[i])));
  canvas.hidden = offline || !range;
  $('legend').replaceChildren();
  $('point-count').textContent = points.length + (report?.hostObservation ? ' 个观测点' : ' 个快照');
  $('trend-empty').hidden = !canvas.hidden;
  $('trend-empty').textContent = offline ? '离线报告只显示当前快照或区间汇总。' : points.length ? '所选频带暂无声压级读数。' : '等待实时声压级读数。';
  if (canvas.hidden) return;
  const [low, high] = range;
  const yOf = v => top + (high - v) / (high - low) * (bottom - top);
  for (let i = 0; i <= 4; i++) {
    const v = high - i * (high - low) / 4, y = yOf(v);
    ctx.fillText(String(v), 2, y + 3); ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke();
  }
  const first = points[0].receivedAt, last = points.at(-1).receivedAt;
  const x = point => points.length === 1 ? left : left + (point.receivedAt - first) / Math.max(1, last - first) * (right - left);
  for (const band of bands) {
    const legend = document.createElement('span'), dot = document.createElement('i');
    dot.style.background = palette[band]; legend.append(dot, MonitorModel.centerLabel(history.bands[band]) + ' Hz'); $('legend').append(legend);
    ctx.strokeStyle = palette[band]; ctx.fillStyle = palette[band]; ctx.lineWidth = 1.6; ctx.beginPath();
    let previous = null;
    for (const point of points) {
      const v = point.values[band]; if (v === null) { previous = null; continue; }
      const y = yOf(v);
      if (!previous || point.receivedAt - previous.receivedAt > 8000) ctx.moveTo(x(point), y); else ctx.lineTo(x(point), y);
      ctx.fillRect(x(point) - 1.5, y - 1.5, 3, 3);
      previous = point;
    }
    ctx.stroke();
  }
  ctx.fillStyle = '#8390a2'; ctx.fillText(new Date(first).toLocaleTimeString(), left, 205);
  if (points.length > 1) { ctx.textAlign = 'right'; ctx.fillText(new Date(last).toLocaleTimeString(), right, 205); }
}
function setExportEnabled(enabled) {
  for (const kind of ['json','csv','html']) $('export-'+kind).disabled = !enabled || !report || exportBusy;
}
async function getJSON(path, options = {}, timeout = 3500) {
  const response = await fetch(path, {...options, cache: 'no-store', signal: AbortSignal.timeout(timeout)});
  const data = /^\/api\/(spectrum|report)(\/|$)/.test(path)
    ? JSON.parse(await response.text()) : await response.json();
  if (!response.ok) {
    const error = new Error(data.error === 'BOARD_NOT_CONFIGURED' ? '请填写板卡地址，或导入已保存报告。' : `请求失败 (${response.status} / ${value(data.error)})`);
    error.httpStatus = response.status; throw error;
  }
  return data;
}
async function readSnapshot(run) {
  let statusError = null;
  try { const data = await getJSON('/api/status'); if (run !== generation) return; renderStatus(data); }
  catch (error) { statusError = error; if (run !== generation) return; $('capture-state').textContent = '状态不可用'; $('storage-state').textContent = '状态不可用'; }
  try {
    const data = await getJSON('/api/spectrum');
    if (run !== generation) return;
    linkUnavailable = false;
    lastReceived = Date.now();
    const activeSession = !statusError && status?.storage?.active ? status.storage.session : null;
    const missing = data.status === 'NO_SPECTRUM' && data.data_source === 'LIVE_SNAPSHOT' && data.live === null;
    const superseded = activeSession != null && data.data_source === 'LIVE_SNAPSHOT' && data.session_id != null &&
      String(activeSession) !== String(data.session_id);
    if (report && !report.live && !statusError && status?.storage?.active === false) {
      // A closed board can still serve its previous FFT. Keep the requested SD report on screen.
      badge('已存报告', 'good'); showError(''); updateFreshness();
      return;
    }
    if (missing || superseded) {
      if (report && activeSession != null && String(activeSession) !== report.session) {
        report = null; history = MonitorModel.createHistory(); lastAdvanced = 0; reportStale = false;
        setExportEnabled(false);
      }
      if (report) {
        reportStale = true;
        setExportEnabled(true);
        if (report.live) {
          $('source').textContent = '等待频谱 · 显示最后一次快照' + (lastReportAt ? '（' + new Date(lastReportAt).toLocaleTimeString() + '）' : '');
          $('band-description').textContent = '最后一次快照的频带声压级（dB）';
        } else {
          $('source').textContent = '板卡已存报告 · 非实时';
          $('band-description').textContent = '已保存区间的频带声压级（dB）';
        }
      } else {
        $('bands').replaceChildren(); $('markers').replaceChildren(); $('legend').replaceChildren();
        $('coverage').textContent = '—'; $('duration').textContent = '—'; $('marker-count').textContent = '0 条';
        $('identity').textContent = 'Session — / Capture — / Config —';
        $('source').textContent = simulation() ? '模拟数据 · 等待频谱' : statusError ? '等待频谱 · 状态接口不可用' : '已连接 · 等待频谱';
        $('band-description').textContent = '当前暂无频带声压级';
        renderFindings(null); modeLabel();
      }
      drawTrend();
    } else {
      const next = MonitorModel.parseReport(data);
      if (MonitorModel.appendHistory(history, next, lastReceived)) {
        history.bands = next.bands;
        lastAdvanced = lastReceived;
        renderReport(next);
      } else if (!reportStale && report && JSON.stringify({...report.raw, published_us:undefined}) ===
                           JSON.stringify({...data, published_us:undefined})) {
        // A publication heartbeat changes the export timestamp, not the visible reading.
        report = next;
      } else {
        renderReport(next);
      }
    }
    badge(statusError ? '部分连接' : simulation() ? '模拟数据' : '已连接', statusError ? 'warn' : 'good');
    showError(statusError ? '频谱接口已响应，状态接口不可用：' + statusError.message : ''); updateFreshness();
  } catch (error) {
    if (run !== generation) return;
    linkUnavailable = Boolean(statusError);
    badge('连接不可用', 'bad'); showError(error.message + (report ? ' · 保留最后结果，请注意数据时间。' : ''));
  }
}
function stopPollTimer() { clearTimeout(pollTimer); pollTimer = null; }
async function poll(run) {
  if (offline || connectionMode === 'offline' || run !== generation || selftestBusy) return;
  if (activePoll) return activePoll;
  stopPollTimer();
  activePoll = readSnapshot(run);
  try { await activePoll; }
  finally {
    activePoll = null;
    if (run === generation && !offline && !selftestBusy) pollTimer = setTimeout(() => poll(run), 2000);
  }
}
function resetSelftest() {
  $('selftest-result').textContent = '尚未检测'; $('selftest-result').className = 'badge';
  $('selftest-time').textContent = '只读检查本地软件与板卡接口';
  $('selftest-checks').replaceChildren();
}
function renderSelftest(data) {
  const labels = {pass: '检查项目通过', attention: '部分项目待确认', fail: '发现异常'};
  if (!labels[data.overall] || !Array.isArray(data.checks)) throw new Error('自检响应格式无效');
  $('selftest-result').textContent = labels[data.overall];
  $('selftest-result').className = 'badge ' + ({pass: 'good', attention: 'warn', fail: 'bad'}[data.overall]);
  $('selftest-time').textContent = '检测时间 ' + new Date(data.checked_at).toLocaleString() + ' · 仅限下列检查项';
  $('selftest-checks').replaceChildren();
  for (const check of data.checks) {
    const row = document.createElement('div'); row.className = 'selftest-check';
    const title = document.createElement('strong'); title.textContent = check.label + ' · ' + ({pass: '通过', wait: '待确认', fail: '异常'}[check.status] ?? '未知');
    const detail = document.createElement('span'); detail.textContent = check.detail;
    row.append(title, detail); $('selftest-checks').append(row);
  }
}
function runSelftest() {
  if (configuring) return;
  if (selftestBusy) return activeSelftest;
  const run = generation;
  selftestBusy = true; stopPollTimer(); $('selftest').disabled = true;
  $('selftest-result').textContent = '检测中…'; $('selftest-result').className = 'badge warn';
  $('selftest-time').textContent = '等待正在进行的读取完成后检测'; $('selftest-checks').replaceChildren();
  activeSelftest = (async () => {
    try {
      // The board serves one connection at a time; finish polling before checking.
      await activePoll;
      if (run !== generation) return;
      const data = await getJSON('/api/selftest', {}, 10000);
      if (run === generation) renderSelftest(data);
    } catch (error) {
      if (run === generation) {
        $('selftest-result').textContent = '自检未完成'; $('selftest-result').className = 'badge bad';
        $('selftest-time').textContent = error.message;
      }
    } finally {
      selftestBusy = false; activeSelftest = null; $('selftest').disabled = configuring;
      if (run === generation) poll(run);
    }
  })();
  return activeSelftest;
}
function updateFreshness() {
  if (offline) { $('freshness').textContent = '离线 · 不更新'; return; }
  if (!lastReceived) return;
  $('received').textContent = '最后接收 ' + new Date(lastReceived).toLocaleTimeString();
  if (!report) { $('freshness').textContent = '等待频谱'; return; }
  if (reportStale) { $('freshness').textContent = report.live ? '等待频谱 · 读数保留' : '已存报告 · 非实时'; return; }
  if (!report?.live) { $('freshness').textContent = '已存报告 · 非实时'; return; }
  if (report.token === null) { $('freshness').textContent = '源新鲜度未知'; return; }
  const age = (Date.now() - lastAdvanced) / 1000;
  if (report.hostObservation) { $('freshness').textContent = `串口读数 · ${age.toFixed(0)} s 前`; return; }
  $('freshness').textContent = age >= 8 ? `已 ${age.toFixed(0)} s 未推进` : `${age.toFixed(0)} s 前推进`;
  if (age >= 8 && $('connection').classList.contains('good')) badge('快照未推进', 'warn');
}
$('file').addEventListener('change', async event => {
  const file = event.target.files[0]; if (!file) return;
  try {
    if (file.size > 1024 * 1024) throw new Error('报告超过 1 MiB 限制');
    const run = generation;
    const next = MonitorModel.parseReport(JSON.parse(await file.text()));
    if (run !== generation) return;
    offline = true; generation++; stopPollTimer(); resetSelftest(); history = MonitorModel.createHistory(); status = null; lastReceived = 0; lastAdvanced = 0;
    configuring = false; $('resume').disabled = false; $('selftest').disabled = selftestBusy;
    renderReport(next); badge('离线查看', 'warn'); showError('');
    $('report-operation').textContent = '已导入 ' + file.name + '，可导出当前报告。';
    $('marker-operation').textContent = '当前显示此文件保存的住户标记。';
    if (typeof renderCaptureControls === 'function') renderCaptureControls();
    $('board').textContent = '本机导入：' + file.name;
    $('capture-state').textContent = '离线报告'; $('storage-state').textContent = '报告快照'; $('storage-detail').textContent = '不代表板卡当前状态';
    $('received').textContent = '文件时间不作采样时间'; updateFreshness();
  } catch (error) { showError('导入失败：' + error.message); }
  event.target.value = '';
});
function resetDisplay() {
  report = null; status = null; history = MonitorModel.createHistory(); lastReceived = 0; lastAdvanced = 0;
  reportStale = false; lastReportAt = 0;
  if (!pendingProduct.marker) $('marker-operation').textContent = '采集开始后可添加标记。';
  $('report-operation').textContent = '导出十个中心频率的声压级；保存后显示文件路径。';
  for (const kind of ['json','csv','html']) $('export-'+kind).disabled = true;
  $('bands').replaceChildren(); $('markers').replaceChildren(); $('legend').replaceChildren();
  $('coverage').textContent = '—'; $('duration').textContent = '—'; $('marker-count').textContent = '0 条';
  $('identity').textContent = 'Session — / Capture — / Config —';
  $('storage-detail').textContent = '等待状态响应';
  $('capture-state').textContent = '未知'; $('storage-state').textContent = '未知';
  $('band-description').textContent = '等待频带声压级';
  renderFindings(null); modeLabel();
  $('freshness').textContent = '尚无数据'; $('received').textContent = '等待首个响应';
  $('source').textContent = '读取连接配置'; badge('准备连接', 'warn'); showError(''); drawTrend();
}
async function connect(configuration) {
  if (!configuring && !offline && connectionMode !== 'offline' &&
      (!configuration || (configuration.mode === connectionMode &&
        (configuration.port || configuration.board?.replace(/\/$/, '')) === connectionBoard))) {
    return poll(generation);
  }
  const run = ++generation;
  configuring = true;
  stopPollTimer(); resetSelftest();
  $('resume').disabled = true; $('selftest').disabled = true;
  try {
    await activeSelftest; await activePoll;
    if (run !== generation) return;
    const data = await getJSON('/api/connection', configuration ? {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(configuration)} : {});
    if (run !== generation) return;
    if (offline || !connectionBoard || data.board !== connectionBoard) resetDisplay();
    offline = false;
    connectionMode = data.mode;
    showTransport();
    if (data.board !== connectionBoard) { pendingProduct.marker = null; pendingProduct.report = null; }
    connectionBoard = data.board;
    if (data.mode !== 'offline') {
      $('transport').value = data.mode;
      showTransport();
      if (data.mode === 'serial') await refreshPorts();
      else $('board-address').value = data.board;
    }
    $('board').textContent = data.board || '正在自动查找板卡…';
    modeLabel();
    if (data.mode === 'offline') {
      $('source').textContent = '离线待命'; badge('未连接', 'warn');
    } else {
      $('source').textContent = '正在连接板卡';
    }
    configuring = false;
    if (run === generation && !report && !archiveRestored) { archiveRestored = true; await restoreArchive(); }
    await runSelftest();
    if (run === generation && !configuration && data.mode === 'offline') await discoverBoard();
  } catch (error) {
    if (run === generation) {
      badge('连接配置未确认', 'warn'); showError(error.message);
      if (configuration && error.httpStatus) {
        if (connectionBoard && connectionMode === 'board') $('board-address').value = connectionBoard;
        if (!offline && connectionMode !== 'offline') pollTimer = setTimeout(() => poll(run), 2000);
      } else {
        connectionMode = 'offline';
      }
    }
  } finally {
    if (run === generation) { configuring = false; $('resume').disabled = false; $('selftest').disabled = selftestBusy; }
  }
}
$('resume').addEventListener('click', async () => {
  if (serialSelected()) {
    const port = $('serial-port').value;
    if (!port) { showError('请接好板卡 USB 串口，点击“刷新串口”后选择 COM 口。'); return; }
    return connect({mode:'serial', port});
  }
  const board = $('board-address').value.trim();
  if (board) return connect({mode: 'board', board});
  if (connectionMode === 'serial') {
    await connect({mode:'offline'});
    if (connectionMode === 'serial') return;
  }
  offline = false;
  return discoverBoard();
});
$('transport').addEventListener('change', () => {
  showTransport();
  if (typeof renderCaptureControls === 'function') renderCaptureControls();
  if (serialSelected()) return refreshPorts();
});
$('serial-port').addEventListener('change', () => {
  if (typeof renderCaptureControls === 'function') renderCaptureControls();
});
$('ports-refresh').addEventListener('click', refreshPorts);
$('serial-release').addEventListener('click', () => connect({mode:'offline'}));
$('selftest').addEventListener('click', runSelftest);
window.addEventListener('resize', drawTrend);
$('trend-band').addEventListener('change', drawTrend);
setInterval(updateFreshness, 1000);
setInterval(() => { if (!connectionBoard || linkUnavailable) discoverBoard(); }, 10000);
connect();

// Product operations share the configured board origin; request IDs permit readback after a lost reply.
let productOperationBusy = false;
const pendingProduct = {marker:null, report:null};
function requestToken() {
  const words = crypto.getRandomValues(new Uint32Array(2));
  return ((BigInt(words[0]) << 32n) | BigInt(words[1]) || 1n).toString();
}
function productControls(busy) {
  productOperationBusy = busy;
  for (const id of ['marker-submit','report-read','capture-start','capture-stop','board-address','resume']) $(id).disabled = busy;
  if (!busy && typeof renderCaptureControls === 'function') renderCaptureControls();
}
async function productRequest(kind, payload) {
  try { return await performProductRequest(kind, payload); }
  catch (error) {
    if (error.httpStatus >= 400 && error.httpStatus < 500) pendingProduct[kind] = null;
    throw error;
  }
}
async function performProductRequest(kind, payload) {
  const pending = pendingProduct[kind];
  let data;
  if (pending) {
    payload = pending;
    try { data = await getJSON('/api/' + kind + '/' + payload.request_id); }
    catch (error) {
      if (error.httpStatus !== 404) throw error;
      data = await getJSON('/api/' + kind, {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
    }
  } else {
    payload = {...payload, request_id:requestToken()};
    pendingProduct[kind] = payload;
    // Retain the request ID on a transport error: querying again never inserts a second marker.
    data = await getJSON('/api/' + kind, {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
  }
  const deadline = Date.now() + 30000;
  while (data.state === 'PENDING') {
    if (Date.now() >= deadline) throw new Error('板端仍在处理；再次点击可查询原操作');
    await new Promise(resolve => setTimeout(resolve, 350));
    data = await getJSON('/api/' + kind + '/' + payload.request_id);
  }
  if (kind === 'marker' && (data.state !== 'QUEUED' || !data.marker_id)) throw new Error('标记状态尚未确认');
  if (kind === 'report') MonitorModel.parseReport(data);
  pendingProduct[kind] = null;
  return data;
}
$('marker-submit').addEventListener('click', async () => {
  if (productOperationBusy) return;
  const note = $('marker-note').value, strength = Number($('marker-strength').value);
  if (!Number.isInteger(strength) || strength < 1 || strength > 5 || new TextEncoder().encode(note).length > 512) {
    $('marker-operation').textContent = '请选择 1–5 级强度，备注最多 512 个 UTF-8 字节。'; return;
  }
  productControls(true); $('marker-operation').textContent = '正在提交标记…';
  try {
    const data = await productRequest('marker', {strength,note});
    $('marker-operation').textContent = '标记 #' + data.marker_id + ' 已提交，等待保存；“停止并保存”完成后写入已存报告。';
    $('marker-note').value = '';
  } catch(error) { $('marker-operation').textContent = '标记未确认：' + error.message + '；再次点击查询原操作。'; }
  finally { productControls(false); }
});
function showStoredReport(next, label, keepLive) {
  if (!keepLive) {
    offline = true; generation++; stopPollTimer(); resetSelftest();
    history = MonitorModel.createHistory(); status = null; lastReceived = 0; lastAdvanced = 0;
  }
  renderReport(next); badge('已存报告', 'good'); showError(''); $('board').textContent = label;
  $('marker-operation').textContent = '下列标记来自板卡已存报告。';
  $('capture-state').textContent = '已存报告'; $('storage-state').textContent = '报告快照';
  $('storage-detail').textContent = keepLive ? '本机留档与板卡当前状态均可用' : '不代表板卡当前状态';
  $('received').textContent = '报告使用板端采样时间';
  $('source').textContent = (keepLive ? '板卡已存报告' : '离线报告') + ' · 非实时';
  updateFreshness();
}
async function readStoredReport(options = {}) {
  if (productOperationBusy || !connectionBoard || (offline && options.auto)) return null;
  const run = generation;
  productControls(true);
  $('report-operation').textContent = options.auto ? '采集已停止，正在自动读取 SD 已存报告…' : '正在读取板卡已存报告…';
  try {
    const data = await productRequest('report', {path: options.path || '', compare_path: options.compare || ''});
    if (run !== generation) return null;
    const label = options.path ? '板卡已存报告：' + options.path : '板卡已存报告 · 本次会话';
    showStoredReport(MonitorModel.parseReport(data), label, connectionMode !== 'offline' && !offline);
    $('report-operation').textContent = '已读取 SD 已存报告，可以导出 JSON、CSV 或打印版。';
    return data;
  } catch (error) {
    if (run !== generation) return null;
    $('report-operation').textContent = 'SD 已存报告读取未完成：' + error.message
      + '\n页面上的最后一份数据仍可导出（本机留档），也可稍后再次点击“读取已存报告”。';
    return null;
  } finally { productControls(false); }
}
async function restoreArchive() {
  if (report) return;
  const run = generation;
  try {
    const data = await getJSON('/api/archive', {}, 6000);
    if (run !== generation || report || !data || !data.report_text) return;
    const next = MonitorModel.parseReport(JSON.parse(data.report_text));
    history = MonitorModel.createHistory();
    history.identity = next.identity;
    history.bands = next.bands;
    if (data.history?.unit === 'spl_db' && data.history.identity === next.identity && Array.isArray(data.history.points)) {
      history.points = data.history.points.filter(point => Number.isFinite(point?.t) &&
        Array.isArray(point.v) && point.v.length === 10 && point.v.every(v => v === null || Number.isFinite(v)))
        .map(point => ({receivedAt:point.t, token:String(point.token ?? ''), values:point.v}));
      history.token = history.points.at(-1)?.token ?? null;
    }
    renderReport(next); reportStale = true;
    lastReportAt = lastAdvanced = Number.isFinite(data.received_at) ? data.received_at : 0;
    const archivedAt = lastReportAt ? new Date(lastReportAt).toLocaleString() : '未知';
    $('source').textContent = '本机留档 · ' + archivedAt + ' · 非实时';
    $('band-description').textContent = '本机留档的频带声压级（dB）';
    $('received').textContent = '留档时间 ' + archivedAt;
    $('freshness').textContent = '上次采集留档';
    $('report-operation').textContent = '已恢复上次采集的数据（本机留档），可直接导出。';
    badge('上次采集留档', 'warn');
    drawTrend();
  } catch (error) { /* 留档是可选的，读取失败不影响实时监测。 */ }
}
$('report-read').addEventListener('click', () => readStoredReport({
  path: $('report-path').value.trim(),
  compare: ($('compare-path').value || '').trim()
}));
function downloadReport(kind, content, session) {
  const type = {json:'application/json', csv:'text/csv', html:'text/html'}[kind];
  const extension = kind === 'html' ? 'html' : kind;
  const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
  const url = URL.createObjectURL(new Blob([content], {type: type + ';charset=utf-8'}));
  const link = document.createElement('a');
  link.href = url; link.download = 'noise-report-' + session + '-' + stamp + '.' + extension;
  document.body.append(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}
async function exportReport(kind) {
  if (!report || exportBusy) return;
  const session = report.session;
  const label = kind === 'html' ? '打印版 HTML' : kind.toUpperCase();
  const content = kind === 'json' ? JSON.stringify({bands:MonitorModel.pressureBands(report)}, null, 2)
    : kind === 'csv' ? MonitorModel.reportCSV(report) : MonitorModel.reportHTML(report);
  exportBusy = true; setExportEnabled(false);
  $('report-operation').textContent = '正在保存 ' + label + '…';
  try {
    const result = await getJSON('/api/export', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body:JSON.stringify({kind, session, content})
    }, 15000);
    $('report-operation').textContent = label + ' 已保存到本机（' + result.bytes + ' 字节）\n' + result.path;
  } catch (error) {
    // 保存目录不可写时仍要把数据交给用户：退回浏览器下载。
    const failure = label + ' 写入保存目录失败：' + error.message;
    try {
      downloadReport(kind, content, session);
      $('report-operation').textContent = failure + '\n已请求下载副本，请确认下载是否完成。';
    } catch (downloadError) {
      $('report-operation').textContent = failure + '\n下载也未能发起：' + downloadError.message;
    }
  } finally { exportBusy = false; setExportEnabled(true); }
}
for (const kind of ['json','csv','html']) $('export-'+kind).addEventListener('click',()=>exportReport(kind));
$('open-folder').addEventListener('click', async () => {
  $('report-operation').textContent = '正在打开报告保存文件夹…';
  try {
    const data = await getJSON('/api/reveal', {
      method:'POST', headers:{'Content-Type':'application/json'}, body:'{}'
    }, 8000);
    $('report-operation').textContent = '已打开报告保存文件夹：' + data.dir;
  } catch (error) {
    $('report-operation').textContent = '无法打开保存文件夹：' + error.message
      + '\n仍可从上方导出（导出成功后会显示完整路径），或用浏览器下载。';
  }
});
