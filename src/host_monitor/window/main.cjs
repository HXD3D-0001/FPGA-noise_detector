'use strict';
const {app, BrowserWindow, dialog} = require('electron');
const {spawn} = require('node:child_process');
const path = require('node:path');
const {createInterface} = require('node:readline');

let win, service, permitted = false, closing = false, pending = null, lastError = '';
function askService(timeout) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending = null;
      reject(new Error('本地服务响应超时。'));
    }, timeout);
    pending = (error, result) => {
      clearTimeout(timer);
      pending = null;
      if (error) reject(error); else resolve(result);
    };
  });
}
function fail(error) {
  if (pending) pending(error);
  if (permitted) return;
  permitted = true;
  dialog.showErrorBox('住宅低频监测 · 本地服务异常', [error.message, lastError.trim(),
    '请完整解压软件后重试。若此前正在采集，请保持板卡供电，重新连接后确认保存状态。'].filter(Boolean).join('\n'));
  app.quit();
}
async function closeWindow(event) {
  if (permitted) return;
  event.preventDefault();
  if (closing) return;
  closing = true;
  try {
    const reply = askService(5000);
    service.stdin.write('{"command":"close"}\n');
    const state = await reply;
    if (state.can_close !== true) {
      const fault = state.phase === 'error' && state.may_be_running;
      const answer = await dialog.showMessageBox(win, {
        type:'warning', buttons:fault ? ['返回监测','退出窗口'] : ['返回监测'], defaultId:0, cancelId:0,
        message:fault ? '板卡保存状态尚未确认。退出后请保持板卡供电。' : '采集或保存仍在进行，请先点击“停止并保存”。'
      });
      if (!fault || answer.response !== 1) return;
    }
    permitted = true;
    service.stdin.end('{"command":"quit"}\n');
    win.destroy();
  } catch (error) {
    if (!permitted) dialog.showErrorBox('未确认保存状态', error.message + ' 请保留窗口，检查板卡连接。');
  } finally { closing = false; }
}
async function start() {
  const runtime = path.join(path.dirname(process.execPath), 'runtime');
  service = spawn(path.join(runtime, 'python.exe'), ['-I','-B','-u',path.join(runtime,'desktop.py'),'--window-service'],
    {cwd:runtime, windowsHide:true, stdio:['pipe','pipe','pipe']});
  const ready = askService(20000);
  service.once('error', fail);
  service.stdin.on('error', error => { if (!permitted) fail(error); });
  service.stderr.setEncoding('utf8');
  service.stderr.on('data', chunk => { lastError = (lastError + chunk).slice(-2000); });
  createInterface({input:service.stdout}).on('line', line => {
    if (!pending) return;
    try { pending(null, JSON.parse(line)); }
    catch (error) { if (pending) pending(error); }
  });
  service.once('close', code => {
    if (!permitted) fail(new Error('本地服务已退出（' + code + '）。'));
    else app.quit();
  });
  const {url} = await ready;
  if (!/^http:\/\/127\.0\.0\.1:\d+$/.test(url)) throw new Error('本地服务未提供有效监听地址。');
  win = new BrowserWindow({width:1180, height:850, minWidth:680, minHeight:520,
    title:'住宅低频噪声排查', backgroundColor:'#edf2f4', autoHideMenuBar:true,
    webPreferences:{sandbox:true, contextIsolation:true, nodeIntegration:false}});
  win.setMenu(null);
  win.webContents.setWindowOpenHandler(() => ({action:'deny'}));
  win.webContents.on('will-navigate', (event, target) => {
    if (new URL(target).origin !== url) event.preventDefault();
  });
  win.on('close', closeWindow);
  await win.loadURL(url);
}
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (win && !win.isDestroyed()) { if (win.isMinimized()) win.restore(); win.show(); win.focus(); }
  });
  // The service exits after flushing its archive; let its close event release the app.
  app.on('window-all-closed', () => {});
  app.whenReady().then(start).catch(error => { if (!permitted) fail(error); });
}
