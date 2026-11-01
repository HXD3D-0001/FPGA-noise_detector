'use strict';
const captureElement = id => document.getElementById(id);
let captureState = {phase: 'idle', board: null, may_be_running: false}, capturePosting = false, captureReadBusy = false, captureUncertain = false;
let captureActionVersion = 0, stopReadPending = false;
async function captureJSON(path, options = {}) {
  const response = await fetch(path, {...options, cache: 'no-store', signal: AbortSignal.timeout(4000)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || '采集操作请求失败');
  return data;
}
function renderCaptureControls() {
  if (typeof productOperationBusy !== 'undefined' && productOperationBusy) return;
  const busy = capturePosting || ['starting', 'stopping'].includes(captureState.phase);
  const board = captureState.board;
  const serial = /^COM\d+$/i.test(board || '');
  const selected = captureElement('transport').value === 'serial';
  const mismatch = selected !== serial || (selected && captureElement('serial-port').value !== board);
  captureElement('capture-start').disabled = !board || mismatch || busy || captureState.may_be_running || captureUncertain || !captureState.confirmed || captureState.phase === 'error';
  const inactive = captureState.confirmed && !captureState.may_be_running &&
    ['idle', 'stopped'].includes(captureState.phase);
  captureElement('capture-stop').disabled = !board || busy || (inactive && !captureUncertain);
  for (const id of ['board-address','transport','serial-port','ports-refresh','serial-release']) {
    captureElement(id).disabled = busy || captureState.may_be_running;
  }
  captureElement('capture-board').textContent = board ? (serial ? '板卡串口：' : '控制与数据地址：') + board : '选择连接方式并连接板卡';
  captureElement('capture-operation').className = 'capture-operation ' + (captureState.phase === 'error' || captureUncertain ? 'capture-error' : '');
  captureElement('recording-warning').textContent = captureState.recording_error || '';
  captureElement('recording-warning').hidden = !captureState.recording_error;
  captureElement('marker-submit').disabled = captureState.phase !== 'running' || captureUncertain || (typeof offline !== 'undefined' && offline);
  captureElement('report-read').disabled = !board || busy || captureState.may_be_running;
}
async function refreshCaptureState() {
  if (capturePosting || captureReadBusy) return;
  const version = captureActionVersion;
  captureReadBusy = true;
  try {
    const state = await captureJSON('/api/capture');
    if (version !== captureActionVersion) return;
    if (!['idle','starting','running','stopping','stopped','error'].includes(state.phase)) throw new Error('采集状态响应格式错误');
    captureState = state; captureUncertain = false;
    if (state.board && state.may_be_running && !/^COM\d+$/i.test(state.board)) captureElement('board-address').value = state.board;
    captureElement('capture-operation').textContent = state.message;
    if (stopReadPending && state.confirmed && ['stopped', 'error', 'idle'].includes(state.phase)) {
      // 停止并保存确认后自动读取本次会话的 SD 汇总，用户不用再手动找路径。
      const stoppedCleanly = state.phase === 'stopped';
      stopReadPending = false;
      if (stoppedCleanly && typeof readStoredReport === 'function') {
        captureElement('capture-operation').textContent = '采集已停止，SD 文件已关闭；正在读取本次会话报告…';
        setTimeout(() => readStoredReport({auto: true}), 200);
      }
    }
  } catch (error) {
    if (version !== captureActionVersion) return;
    captureUncertain = true;
    captureElement('capture-operation').textContent = '操作状态未确认：' + error.message + '。请保持板卡供电，可在同一地址重连后停止并保存。';
  } finally { captureReadBusy = false; renderCaptureControls(); }
}
async function submitCapture(action) {
  if (capturePosting || ['starting','stopping'].includes(captureState.phase)) return;
  captureActionVersion++;
  capturePosting = true; renderCaptureControls();
  try {
    captureState = await captureJSON('/api/capture', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({action})});
    captureUncertain = false;
    captureElement('capture-operation').textContent = captureState.message;
    if (action === 'stop') stopReadPending = true;
    if (action === 'start' && typeof connect === 'function') connect();
  } catch (error) {
    captureUncertain = true;
    captureElement('capture-operation').textContent = '操作未确认：' + error.message + '。可重试停止并保存。';
  } finally { capturePosting = false; renderCaptureControls(); }
}
captureElement('capture-start').addEventListener('click', () => submitCapture('start'));
captureElement('capture-stop').addEventListener('click', () => submitCapture('stop'));
setInterval(refreshCaptureState, 1000);
refreshCaptureState();
