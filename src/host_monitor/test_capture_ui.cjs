const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
function harness() {
  class Element {
    constructor() { this.value = ''; this.children = []; this.handlers = {}; }
    append(...items) { this.children.push(...items); }
    replaceChildren(...items) { this.children = items; }
    addEventListener(event, callback) { this.handlers[event] = callback; }
  }
  const nodes = new Map();
  const element = id => { if (!nodes.has(id)) nodes.set(id, new Element()); return nodes.get(id); };
  const state = {phase:'idle', may_be_running:false, board:'http://169.254.70.21', confirmed:true, message:'板卡就绪'}, calls=[];
  let fail = false;
  const context = vm.createContext({document:{getElementById:element, createElement:() => new Element()},
    setInterval(){}, AbortSignal:{timeout(){}}, fetch:async (url, options={}) => {
      calls.push({url, options});
      if (fail) throw new Error('连接中断');
      if (options.method === 'POST') {
        const action=JSON.parse(options.body).action;
        state.phase=action==='start'?'starting':'stopping'; state.board='http://169.254.70.21';
        state.message=action==='start'?'正在准备 ADC 采集…':'正在停止并等待 SD 文件关闭…';
      }
      return {ok:true,json:async () => ({...state})};
    }});
  vm.runInContext(fs.readFileSync(path.join(__dirname,'capture_ui.js'),'utf8'),context);
  return {element,state,calls,context,setFail:value=>{fail=value;}};
}
const flush = () => new Promise(resolve => setImmediate(resolve));

test('product controls issue only ADC start and wait for real confirmation', async () => {
  const h=harness(); await flush();
  assert.match(h.element('capture-board').textContent,/169.254.70.21/);
  assert.equal(h.calls.filter(c=>c.options.method==='POST').length,0);
  await h.element('capture-start').handlers.click();
  assert.equal(h.element('capture-start').disabled,true);
  assert.match(h.element('capture-operation').textContent,/准备/);
  assert.doesNotMatch(h.element('capture-operation').textContent,/已确认/);
  const sent=JSON.parse(h.calls.find(c=>c.options.method==='POST').options.body);
  assert.deepEqual(sent,{action:'start'});
  h.state.phase='running';h.state.may_be_running=true;h.state.message='ADC 左声道采集已确认';
  await vm.runInContext('refreshCaptureState()',h.context);
  assert.equal(h.element('capture-start').disabled,true);
  assert.equal(h.element('capture-stop').disabled,false);
  await h.element('capture-stop').handlers.click();
  assert.match(h.element('capture-operation').textContent,/等待 SD/);
  assert.equal(h.element('capture-stop').disabled,true);
  h.state.phase='stopped';h.state.may_be_running=false;h.state.message='SD 文件已关闭';
  await vm.runInContext('refreshCaptureState()',h.context);
  assert.equal(h.element('capture-start').disabled,false);
});

test('control transport errors stay visible and never claim stopped', async () => {
  const h=harness();await flush();h.setFail(true);
  await h.element('capture-start').handlers.click();
  assert.match(h.element('capture-operation').textContent,/中断/);
  assert.doesNotMatch(h.element('capture-operation').textContent,/已停止|已保存/);
  assert.equal(h.element('capture-start').disabled,true);
});

test('a poll sent before START cannot replace the submitted operation', async () => {
  const h = harness(); await flush();
  const network = h.context.fetch;
  let finish;
  h.context.fetch = (url, options) => options.method === 'POST' ? network(url, options) :
    new Promise(resolve => { finish = resolve; });
  const old = vm.runInContext('refreshCaptureState()', h.context);
  const idle = {...h.state};
  await h.element('capture-start').handlers.click();
  finish({ok:true, json:async () => idle});
  await old;
  assert.match(h.element('capture-operation').textContent, /准备/);
  assert.equal(h.element('capture-start').disabled, true);
});

test('production markup exposes normal capture operations without test/debug inputs', () => {
  const html=fs.readFileSync(path.join(__dirname,'index.html'),'utf8');
  assert.match(html,/id="capture-start"[^>]*>开始采集/);
  assert.match(html,/id="capture-stop"[^>]*>停止并保存/);
  assert.match(html,/id="serial-port"/);
  assert.doesNotMatch(html,/STARTADC|START &lt;|调试按钮|id="demo"/);
});


