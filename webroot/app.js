// 知言管理页：服务通了没有 → 断在哪一环 → 下一步做什么，然后是接入信息、设置、开关、更新与诊断。
// 判定规则沿用原知言应用的 Status：活数据拿不到时绝不把「在线」当成现在的结论；同一个微信进程里最近一次的应答、
// 或模块自己发布的状态文件，只在微信被冻结、服务暂停的这段时间里沿用，并且明说是暂停中。
import {
  ctl, fromB64, toB64, parseJson, h, icon, setText, section, note, group, item, switchItem, button, iconButton,
  hero as makeHero, dataList, snack, confirmDialog, copy, topBar, poll, duration, ago, updates,
} from './kit.js';

const DEFAULT_PORT = 5601;
const PAUSE_GRACE_MS = 30_000;
const OLD_MODULE_PROBE_MS = 16 * 60_000;

// ---------------------------------------------------------------- 配置（与 native/server.cpp 的 ReadConfig 判得一样）

const TOKEN_RE = /^[A-Za-z0-9_-]{32,128}$/;

function parseConf(text) {
  if (text == null) return { error: '配置文件不存在' };
  if (new TextEncoder().encode(text).length > 1023) return { error: '配置文件超过 1023 字节' };
  let port = null, token = null;
  const seen = new Set();
  for (const raw of text.split('\n')) {
    const line = raw.endsWith('\r') ? raw.slice(0, -1) : raw;
    if (!line || line[0] === '#') continue;
    const eq = line.indexOf('=');
    const key = eq < 0 ? line : line.slice(0, eq);
    const value = eq < 0 ? '' : line.slice(eq + 1);
    if (seen.has(key)) return { error: `「${key}」重复出现` };
    seen.add(key);
    if (key === 'token') {
      if (!TOKEN_RE.test(value)) return { error: '令牌须为 32–128 位英文字母、数字、- 或 _' };
      token = value;
    } else if (key === 'port') {
      if (!/^\d+$/.test(value) || +value < 1024 || +value > 65535) return { error: '端口须为 1024–65535 的整数' };
      port = +value;
    } else if (key === 'send') {
      // 已取消的开关：值仍须是 on 或 off，但不起作用。
      if (value !== 'on' && value !== 'off') return { error: 'send 只能是 on 或 off' };
    } else if (key !== 'send_allow') {
      return { error: `无法识别的设置「${key.length > 24 ? key.slice(0, 24) + '…' : key}」` };
    }
  }
  if (token == null) return { error: '缺少令牌（token）' };
  return { port: port ?? DEFAULT_PORT, token };
}

function newToken() {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

// ---------------------------------------------------------------- 一次检查

/** 由 ctl.sh probe 的输出得到的快照，外加跨次检查才知道的事（连续失败的起点、同一进程里最近一次应答）。 */
const S = { checked: false, restarting: false, bridge: true, d: null, conf: null, confError: '', status: null, meta: null,
  http: 0, failure: '', failSince: 0, carried: null, published: null, wechatVersion: '', installed: true, now: 0 };

function readProbe(r) {
  const p = r.kv;
  S.checked = true;
  S.bridge = r.bridge;
  if (!r.bridge || p.module === undefined && p.version === undefined) {
    S.d = null;
    return p;
  }
  S.d = {
    module: p.module === '1', disabled: p.disabled === '1', removing: p.removing === '1', updatePending: p.update === '1',
    version: p.version || '', updateVersion: p.update_version || '', enabled: p.enabled !== '0',
    pid: Number(p.pid) || 0, frozen: p.frozen === '1' && Number(p.pid) > 0, authed: !!p.auth && p.auth !== '0',
    guard: p.guard || '', guardReason: p.guard_reason || '', guardWatching: p.guard_watch === '1',
  };
  const conf = parseConf(fromB64(p.conf));
  S.conf = conf.error ? null : conf;
  S.confError = conf.error || '';
  S.installed = p.installed === '1';
  S.wechatVersion = p.wechat_version || '';
  S.http = Number(p.http) || 0;
  S.failure = p.curl === '7' ? 'refused' : p.curl === '28' ? 'timeout' : p.http ? 'other' : '';
  S.status = S.http === 200 ? parseJson(fromB64(p.status)) : null;
  S.meta = S.http === 200 ? parseJson(fromB64(p.meta)) : null;
  S.published = parseJson(fromB64(p.published));
  S.now = (Number(p.now) || 0) * 1000 || Date.now();
  if (S.status) {
    S.failSince = 0;
    S.carried = { pid: S.d.pid, status: S.status, meta: S.meta };
  } else if (!S.failSince) {
    S.failSince = S.now;
  }
  return p;
}

const serving = () => S.http === 200 && !!S.status;
const guardPaused = () => S.d?.guard === 'PAUSED';
const port = () => S.conf?.port ?? DEFAULT_PORT;

/** 模块发布的状态，仅当它属于眼下这个微信进程（pid 对得上）。 */
function published() {
  const p = S.published;
  return p && S.d && S.d.pid > 0 && p.pid === S.d.pid ? p : null;
}

function carried() {
  return !serving() && S.carried && S.d && S.d.pid > 0 && S.carried.pid === S.d.pid;
}

const statusView = () => (serving() ? S.status : carried() ? S.carried.status : null);
const metaView = () => (serving() ? S.meta : carried() ? S.carried.meta : null);
const known = () => !!statusView() || !!published();

function paused() {
  if (S.d.frozen) return true;
  return S.failure === 'timeout' && known() && (S.failSince <= 0 || S.now - S.failSince < PAUSE_GRACE_MS);
}

function sendState() {
  const status = statusView();
  const send = status?.send;
  if (send) {
    if (send.state) return send.state;
    if (send.dispatcher) return 'ready';
    if (send.resolved) return 'waiting';
    return (status.keepalive?.uptime_ms || 0) > OLD_MODULE_PROBE_MS ? 'incompatible' : 'unready';
  }
  return published()?.send?.state || '';
}

function login() {
  const logins = metaView()?.logins;
  for (const l of logins || []) if (l && l.status === 1 && l.user) return l;
  if (serving()) return null;
  const who = published()?.login;
  if (!who || !(who.online > 0) || !who.id) return null;
  return { status: 1, user: { id: who.id, name: who.name, nick: who.name } };
}

const displayName = (l) => (l ? l.user.nick || l.user.name || '' : '');

/** 运行中的服务监听的端口与文件里的不同：新端口还没生效。 */
function portPending() {
  const p = published();
  return !!(p && S.conf && p.listening && p.port && p.port !== S.conf.port);
}

function state() {
  if (!S.checked) return 'checking';
  if (S.restarting) return 'restarting';
  const d = S.d;
  if (!d) return 'no-root';
  if (!d.module) return d.updatePending ? 'module-pending' : 'no-module';
  if (d.disabled || d.removing) return 'module-off';
  if (!d.enabled) return 'switched-off';
  if (!S.conf) return 'config-bad';
  if (serving()) {
    if (login()) return sendState() === 'incompatible' ? 'ready-no-send' : 'ready';
    return d.authed ? 'account-unseen' : 'logged-out';
  }
  if (S.http === 401 || S.http === 403 || portPending()) return 'config-pending';
  if (!S.installed) return 'no-wechat';
  if (d.pid <= 0) return 'wechat-stopped';
  if (paused()) return 'wechat-frozen';
  return 'service-down';
}

// ---------------------------------------------------------------- 动作

async function act(args, busy, done) {
  const r = await ctl(...args);
  if (!r.ok || r.kv.error) snack(r.kv.error || busy);
  else if (done) snack(done);
  refresh();
  return r;
}

const openWeChat = () => act(['open'], '没能打开微信');
const wechatSettings = () => act(['app-settings'], '没能打开应用详情');

async function restartWeChat() {
  S.restarting = true;
  render();
  await ctl('restart');
  setTimeout(() => { S.restarting = false; refresh(); }, 3000);
}

async function setGuard(on) {
  await act(['guard', on ? 'start' : 'stop'], on ? '保活看守没有开启' : '保活看守没有暂停', on ? '保活看守已开启' : '保活看守已暂停');
}

async function setEnabled(on) {
  await act(['set-enabled', on ? '1' : '0'], '没能写入模块开关', on ? '模块已开启' : '模块已关闭');
}

async function setWakelock(on) {
  await act(['wakelock', on ? 'on' : 'off'], '服务没有响应', on ? '唤醒锁已获取' : '唤醒锁已释放');
}

// ---------------------------------------------------------------- 结论

function heroLine() {
  const st = state();
  const d = S.d;
  const A = {
    open: { label: '打开微信', run: openWeChat },
    restart: { label: '重新启动微信', run: restartWeChat },
    settings: { label: '后台运行设置', run: wechatSettings },
    guard: { label: '开启保活看守', run: () => setGuard(true) },
    on: { label: '开启模块', run: () => setEnabled(true) },
    fix: { label: '修复配置', run: () => { portField.focus(); portField.scrollIntoView({ block: 'center' }); } },
  };
  switch (st) {
    case 'checking': return { tone: 'neutral', title: '正在检查', detail: '正在连接微信里的知言服务…' };
    case 'restarting': return { tone: 'neutral', title: '正在重新启动微信', detail: '新设置随微信启动生效；服务恢复后，客户端会自动重连。' };
    case 'no-root': return { tone: 'error', title: '需要在 root 管理器里打开', detail: '这个页面通过 KernelSU / APatch 的模块网页界面执行 root 命令；在模块列表里点知言打开它。' };
    case 'module-pending': return { tone: 'warning', title: '重启手机后启用', detail: '知言模块已经刷入，重启手机后生效。' };
    case 'no-module': return { tone: 'error', title: '没有安装知言模块', detail: '用 KernelSU 或 Magisk 刷入知言模块包（satori-wx-server），然后重启手机。模块依赖 Zygisk。' };
    case 'module-off': return d.removing
      ? { tone: 'warning', title: '模块将被卸载', detail: '知言模块已标记为卸载，重启手机后移除。要保留它，在模块列表里撤销卸载。' }
      : { tone: 'warning', title: '模块已停用', detail: '在模块列表里重新启用知言，然后重启手机。' };
    case 'switched-off': return { tone: 'neutral', title: '已关闭', detail: '模块开关已关闭：不监听端口、不显示通知、不保活。关闭微信不会受影响。', actions: [A.on] };
    case 'config-bad': return { tone: 'error', title: '配置文件无效', detail: `${S.confError || '配置无法读取'}。服务端不会启动；在下面的设置里重新保存一次即可修复。`, actions: [A.fix] };
    case 'no-wechat': return { tone: 'error', title: '没有安装微信', detail: '知言运行在微信里，请先安装微信。' };
    case 'wechat-stopped': return { tone: 'warning', title: '微信没有在运行', detail: '知言服务随微信启动。系统在后台回收了微信时，服务也会一起停下。', actions: [A.open] };
    case 'wechat-frozen': {
      const name = displayName(login());
      const title = d.frozen ? '微信被系统冻结了' : '服务暂时没有回应';
      const detail = d.frozen
        ? `微信退到后台后被系统冻结，知言服务随之暂停${name ? `，冻结前已登录「${name}」` : ''}。有新消息、或你打开微信时，系统会唤醒它，服务随即恢复；这期间客户端的请求会等一等。`
        : `微信在运行，知言服务之前一直正常${name ? `（已登录「${name}」）` : ''}，但这会儿没有应答——多半是刚被系统冻结或正在解冻，几秒内会恢复。超过半分钟还没恢复，这里会改成提示重新启动微信。`;
      return guardPaused()
        ? { tone: 'warning', title, detail: detail + '保活看守已暂停，开启后它会在微信被冻结的瞬间解冻。', actions: [A.open, A.guard] }
        : { tone: 'warning', title, detail: detail + '要减少被冻结，在微信的应用详情里允许它后台运行（「耗电管理」一项）。', actions: [A.open, A.settings] };
    }
    case 'service-down': return { tone: 'error', title: '服务没有响应',
      detail: d.updatePending ? '模块更新已经刷入但还没生效。重启手机后再试。'
        : `微信在运行，但端口 ${port()}${S.failure === 'refused' ? ' 拒绝连接（服务没有启动）。' : ' 没有回应。'}刚刷入模块时需要重启手机；否则重新启动微信通常可以恢复。`,
      actions: [A.restart] };
    case 'config-pending': return { tone: 'warning', title: '新设置尚未生效', detail: '服务仍在使用旧的端口或令牌。重新启动微信后生效；客户端也要换成新的设置。', actions: [A.restart] };
    case 'account-unseen': return { tone: 'error', title: '服务没认出已登录的账号', detail: '微信里已经登录，但知言模块读不到账号身份——微信改了登录信息的存放位置。在模块修好之前，客户端收不到消息、也发不出去。复制诊断报告可以帮助排查。' };
    case 'logged-out': return { tone: 'warning', title: '等待登录微信', detail: '服务已就绪。在微信里登录账号后，客户端就能收到消息。', actions: [A.open] };
    case 'ready-no-send': {
      const name = displayName(login());
      const v = serviceVersion();
      return { tone: 'warning', title: '可以接收，暂不能发送',
        detail: `${name ? `已登录「${name}」。` : '微信已登录。'}微信${S.wechatVersion ? ' ' + S.wechatVersion : ''} 比知言模块${v && v !== '?' ? `（v${v}）` : ''}适配的版本新，发送、撤回、拍一拍、群管理等操作暂时不可用；接收消息不受影响。等模块适配这个微信版本后恢复。` };
    }
    default: {
      const name = displayName(login());
      return { tone: 'success', title: '服务就绪', detail: `${name ? `已登录「${name}」。` : '微信已登录。'}客户端可以收发消息。` };
    }
  }
}

function serviceVersion() {
  return statusView()?.version || published()?.version || '?';
}

const STEP = { ok: ['ok', 'ok'], wait: ['pending', 'wait'], fail: ['error', 'fail'], unknown: ['info', ''] };

function steps() {
  const st = state();
  if (['switched-off', 'checking', 'restarting', 'no-root'].includes(st)) return ['unknown', 'unknown', 'unknown', 'unknown'];
  const d = S.d, on = serving(), isPaused = st === 'wechat-frozen';
  const wechat = !S.installed ? 'fail' : on ? 'ok' : d.frozen || isPaused ? 'wait' : d.pid > 0 ? 'ok' : 'wait';
  const service = on ? 'ok' : st === 'config-pending' ? 'wait' : ['wechat-stopped', 'no-wechat'].includes(st) ? 'unknown' : isPaused ? 'wait' : 'fail';
  const k = on || (isPaused && known());
  const l = k ? login() : null;
  const account = !k ? 'unknown' : l ? 'ok' : on ? (d.authed ? 'fail' : 'wait') : 'unknown';
  const send = { ready: 'ok', waiting: 'wait', unready: 'wait', incompatible: 'fail' }[k ? sendState() : ''] || 'unknown';
  return [wechat, service, account, send];
}

function stepValues() {
  const st = state();
  const d = S.d;
  if (st === 'switched-off') return [!S.installed ? '未安装' : d.pid > 0 ? '运行中' : '没有在运行', '模块已关闭', '—', '—'];
  if (st === 'checking' || st === 'restarting') return ['检查中', '检查中', '检查中', '检查中'];
  if (st === 'no-root') return ['未知', '未知', '未知', '未知'];
  const on = serving(), isPaused = st === 'wechat-frozen';
  const v = S.wechatVersion ? ` · ${S.wechatVersion}` : '';
  const wechat = !S.installed ? '未安装' : on ? `运行中${v}`
    : isPaused ? `运行中${v}${d.frozen ? ' · 后台被系统冻结' : ' · 暂时没有应答'}` : d.pid > 0 ? `运行中${v}` : '没有在运行';
  let service;
  if (on) {
    const up = S.status.keepalive?.uptime_ms || 0;
    service = `127.0.0.1:${port()} · v${S.status.version || '?'}${up >= 60_000 ? ` · 已在线 ${duration(up)}` : ''}`;
  } else if (st === 'config-pending') service = '在运行，新设置待生效';
  else if (st === 'no-module' || st === 'module-pending') service = '模块未安装';
  else if (st === 'module-off') service = '模块已停用';
  else if (st === 'config-bad') service = '配置无效，不会启动';
  else if (st === 'wechat-stopped' || st === 'no-wechat') service = '随微信启动';
  else if (isPaused) service = known() ? (d.frozen ? `已暂停 · v${serviceVersion()} · 随微信冻结` : `暂时没有应答 · v${serviceVersion()}`)
    : d.frozen ? '随微信冻结而暂停' : '暂时没有应答';
  else service = `端口 ${port()}${S.failure === 'refused' ? ' 拒绝连接' : ' 没有回应'}`;
  const k = on || (isPaused && known());
  const l = k ? login() : null;
  const account = !k ? '—' : !l ? (!on ? '—' : d.authed ? '微信已登录，但服务没认出' : '未登录')
    : (displayName(l) ? `${displayName(l)}（${l.user.id}）` : l.user.id);
  return [wechat, service, account, sendText(k)];
}

function sendText(k) {
  switch (k ? sendState() : '') {
    case 'ready': return '就绪';
    case 'waiting': return '等待微信网络层';
    case 'unready': return '发送功能未就绪（可能与当前微信版本不兼容）';
    case 'incompatible': return `与微信${S.wechatVersion ? ' ' + S.wechatVersion : ''} 不兼容，暂不能发送`;
    default: return '—';
  }
}

function reason(error) {
  if (error === 'empty target or content') return '目标或内容为空';
  if (error === 'JavaVM unavailable') return '微信运行环境未就绪';
  if (error.startsWith('network dispatcher unavailable')) return '微信网络层未就绪';
  if (error.startsWith('dispatch rejected')) return '微信拒绝派发';
  return error || '原因未知';
}

function guardText() {
  const d = S.d;
  if (!d || !d.guard) return '—';
  if (guardPaused()) return `已暂停${d.guardReason === 'user-stop' ? '（手动停止）' : ''} · 微信被冻结后不会自动解冻`;
  return `已开启${d.guardWatching ? ' · 微信被冻结后立即解冻' : ''}`;
}

function diagnostics() {
  const d = S.d;
  let module = !d ? '未知' : !d.module ? '未安装' : d.version || '已安装';
  if (d?.updateVersion) module += `（${d.updateVersion} 待重启手机）`;
  const on = serving();
  const status = statusView();
  const p = published();
  let running = on ? `v${S.status.version || '?'}` : d && !d.enabled ? '已停止（模块开关关闭）'
    : status || p ? `v${serviceVersion()}（暂停中，沿用暂停前的状态）` : '未连接';
  if (on && d?.version && d.version !== `v${S.status.version}`) running += '（与已安装的模块不同，重启手机后更新）';
  let methods = '—';
  const l = on ? login() : null;
  if (l) methods = `${(l.features || []).length} / ${S.status.standard_methods || 37} 个标准方法`;
  const send = status?.send;
  const counts = send ? `已发 ${send.sent || 0} · 失败 ${send.failed || 0} · 撤回 ${send.recalled || 0}` : '—';
  const last = send && 'last_age_ms' in send
    ? `${ago(send.last_age_ms)} · ${send.last_ok ? '已交给微信派发' : '未发出：' + reason(send.last_error || '')}` : '—';
  let capability = sendText(!!(status || p));
  if (send?.detail && sendState() === 'incompatible') capability += `（${send.detail}）`;
  const keep = status?.keepalive;
  const keepalive = !keep ? '—' : `${keep.notification ? '常驻通知已发布' : '常驻通知未发布'} · 唤醒锁${keep.wakelock ? '开启' : '关闭'}`
    + ('cpu_held' in keep ? `（CPU ${keep.cpu_held ? '持有' : '释放'} · Wi-Fi ${keep.wifi_held ? '持有' : '释放'}）` : '');
  const changed = p?.changed_at;
  return [
    !d ? '未知' : d.enabled ? '开启' : '已关闭',
    module,
    running,
    !S.installed ? '未安装' : S.wechatVersion || '已安装',
    methods,
    capability,
    counts,
    last,
    keepalive,
    guardText(),
    !p ? '—' : changed ? `已发布 · ${ago(Math.max(0, S.now - changed))}变化` : '已发布',
    on && S.status.event_replay ? `断线后补发最近 ${S.status.replay_capacity} 条` : '—',
  ];
}

const DIAG = ['模块开关', '已安装的模块', '运行中的服务', '微信', '协议方法', '发送能力', '发送计数', '上次发送', '保活', '保活看守', '状态发布', '事件回放'];

function report() {
  const lines = ['知言 · 诊断报告', `检查时间：${new Date().toLocaleString('zh-CN', { hour12: false })}`, `结论：${heroLine().title}`];
  diagnostics().forEach((v, i) => lines.push(`${DIAG[i]}：${v}`));
  if (S.conf) lines.push(`配置：端口 ${S.conf.port}`);
  else if (S.confError) lines.push(`配置：无效（${S.confError}）`);
  lines.push('报告不含令牌、微信号、昵称或消息内容。');
  return lines.join('\n');
}

// ---------------------------------------------------------------- 页面

const refreshBtn = iconButton('refresh', '刷新状态', () => refresh());
const heroCard = makeHero('知言服务');
const chain = ['微信', '知言服务', '微信账号', '消息发送'].map((t) => item({ icon: 'info', title: t, sub: '检查中' }));
const endpoint = item({ title: `http://127.0.0.1:${DEFAULT_PORT}`, sub: '服务地址 · 只供本机客户端',
  trailing: iconButton('copy', '复制服务地址', () => copy(`http://127.0.0.1:${port()}`, '服务地址')) });
const tokenRow = item({ icon: 'key', title: '令牌', sub: '读取中',
  trailing: iconButton('copy', '复制令牌', () => (S.conf ? copy(S.conf.token, '令牌') : snack('读不到令牌：需要有效的配置'))) });

const portField = h('input', { inputmode: 'numeric', autocomplete: 'off', placeholder: String(DEFAULT_PORT) });
const tokenField = h('input', { type: 'password', autocomplete: 'off', spellcheck: 'false', class: 'mono' });
const portHint = h('span', { class: 'hint', text: '1024–65535。改了端口，客户端里的地址也要一起改。' });
const tokenHint = h('span', { class: 'hint', text: '32–128 位英文字母、数字、- 或 _。客户端必须填写相同的令牌。' });
const eye = iconButton('eye', '显示令牌', () => {
  const show = tokenField.type === 'password';
  tokenField.type = show ? 'text' : 'password';
  eye.replaceChildren(icon(show ? 'eyeOff' : 'eye'));
  eye.setAttribute('aria-label', show ? '隐藏令牌' : '显示令牌');
});
const saveBtn = button('保存', 'filled', save, 'check');
const appliedText = h('p', { class: 'note' });
/** 表单最近一次从文件载入的值；与它不同即为草稿，草稿期间轮询不覆盖输入。 */
let formBase = { port: '', token: '' };

const moduleSwitch = switchItem({ icon: 'power', title: '模块开关', onChange: setEnabled });
const guardSwitch = switchItem({ icon: 'shield', title: '保活看守', onChange: setGuard });
const wakeSwitch = switchItem({ icon: 'bolt', title: '唤醒锁', onChange: setWakelock });
const openRow = item({ icon: 'open', title: '打开微信', sub: '服务随微信主进程运行', onClick: openWeChat });
const restartRow = item({ icon: 'restart', title: '重新启动微信', sub: '让新设置生效；客户端会短暂断开', onClick: restartWeChat });
const bgRow = item({ icon: 'settings', title: '后台运行设置', sub: '打开微信的应用详情，允许它后台运行，减少被系统冻结', onClick: wechatSettings });
const diag = dataList(DIAG);
const up = updates('知言', () => refresh());

function fieldBox(label, input, trailing) {
  return h('label', { class: 'field' }, input, h('span', { text: label }), trailing);
}

document.body.append(
  topBar('知言', refreshBtn),
  h('main', {},
    h('div', { class: 'brand' }, h('h2', { text: '知言' }), h('p', { text: '微信的 Satori 服务' })),
    up.cards,
    h('div', { style: 'margin-top:16px' }, heroCard.el),
    section('连接链路'), group(...chain),
    section('客户端接入'), group(endpoint, tokenRow),
    note('Satori 客户端填这个地址即可，版本段 /v1 由客户端自己拼上（接口 /v1/…，事件 /v1/events）；令牌作为 Bearer 鉴权。'),
    section('连接设置'),
    h('div', { class: 'form' },
      fieldBox('本机端口', portField), portHint,
      fieldBox('令牌', tokenField, eye), tokenHint,
      h('div', { class: 'row' }, saveBtn,
        button('生成新令牌', 'tonal', () => { tokenField.value = newToken(); validate(); snack('已生成新令牌。保存并重启微信后，客户端也要换成它。'); }, 'key'))),
    appliedText,
    section('模块'), group(moduleSwitch),
    section('微信'), group(openRow, restartRow, guardSwitch, wakeSwitch, bgRow),
    note('保活看守在 root 侧运行：微信被系统冻结时立刻解冻，微信退出了就拉起。唤醒锁由模块在微信里持有，耗电会增加。'),
    section('更新'), up.list,
    note('更新来自 GitHub 发布页，下载后校验 sha256，再由 root 管理器安装。模块更新需要重启手机才会生效。'),
    section('诊断'), diag.el,
    h('div', { class: 'row' }, button('复制报告', 'tonal', () => copy(report(), '诊断报告'), 'copy')),
    note('报告只含版本、状态与计数，不含令牌、微信号、昵称或消息内容。'),
    h('p', { class: 'note footer', text: '关闭这个页面不影响服务：服务在微信里运行。' }),
  ),
);

portField.addEventListener('input', validate);
tokenField.addEventListener('input', validate);

function dirty() {
  return portField.value !== formBase.port || tokenField.value !== formBase.token;
}

function validate() {
  const p = portField.value.trim();
  const portOk = /^\d+$/.test(p) && +p >= 1024 && +p <= 65535;
  const tokenOk = TOKEN_RE.test(tokenField.value);
  portField.parentElement.classList.toggle('invalid', !!p && !portOk);
  tokenField.parentElement.classList.toggle('invalid', !!tokenField.value && !tokenOk);
  portHint.className = `hint${p && !portOk ? ' error' : ''}`;
  tokenHint.className = `hint${tokenField.value && !tokenOk ? ' error' : ''}`;
  setText(tokenHint, tokenField.value && !tokenOk
    ? (tokenField.value.length < 32 ? `令牌太短：至少 32 位，现在 ${tokenField.value.length} 位` : '令牌只能用英文字母、数字、- 或 _')
    : '32–128 位英文字母、数字、- 或 _。客户端必须填写相同的令牌。');
  saveBtn.disabled = !(portOk && tokenOk) || !S.d?.module || (!!S.conf && !dirty());
  return portOk && tokenOk;
}

async function save() {
  if (!validate()) return;
  saveBtn.disabled = true;
  const text = `# 由知言管理页写入；未知键或非法值会让服务端拒绝启动。\nport=${+portField.value.trim()}\ntoken=${tokenField.value}\n`;
  const r = await ctl('write-conf', toB64(text));
  if (r.kv.ok === '1') {
    snack('已保存。重新启动微信后生效');
    portField.value = String(+portField.value.trim());
    formBase = { port: portField.value, token: tokenField.value };
  } else {
    snack(r.kv.error || '保存失败');
  }
  refresh();
}

function appliedLine() {
  if (!S.d) return '';
  if (!S.d.module) return '安装模块并重启手机后才能保存设置。';
  if (!S.conf) return `配置文件无效：${S.confError || '配置无法读取'}。保存一次即可用上面的设置重写它。`;
  if (state() === 'config-pending') return '已保存，重新启动微信后生效：服务仍在使用旧的端口或令牌。';
  if (serving()) return '设置已生效：微信里的服务正在使用这份配置。';
  return '保存后，重新启动微信时生效。';
}

function render() {
  heroCard.set(heroLine());
  const st = steps(), values = stepValues();
  chain.forEach((row, i) => row.lead(...STEP[st[i]]).sub(values[i]));

  endpoint.title(`http://127.0.0.1:${port()}`);
  tokenRow.sub(S.conf ? `已设置 · ${S.conf.token.length} 位` : S.d ? '不可用' : '读取中');

  // 输入框只在没有草稿时跟着文件走，轮询不会冲掉正在输入的内容。
  if (!dirty() && document.activeElement !== portField && document.activeElement !== tokenField) {
    portField.value = S.conf ? String(S.conf.port) : String(DEFAULT_PORT);
    tokenField.value = S.conf ? S.conf.token : '';
    formBase = { port: portField.value, token: tokenField.value };
  }
  validate();
  setText(appliedText, appliedLine());

  const d = S.d;
  const off = !d || !d.module;
  moduleSwitch.checked(d?.enabled ?? true).disabled(off)
    .sub(!d ? '读取中' : d.enabled ? '开启：服务、通知与保活都在运行' : '已关闭：端口、通知与保活都已停下');
  const guardOn = !!d?.guard && !guardPaused();
  guardSwitch.checked(guardOn).disabled(off || !d.guard || !d.enabled)
    .sub(!d?.guard ? '没有找到保活看守' : guardPaused() ? '已暂停：微信退到后台会被系统冻结，服务随之暂停' : '微信被系统冻结后立即解冻，微信退出了就拉起');
  const keep = serving() ? S.status.keepalive : null;
  wakeSwitch.checked(!!keep?.wakelock).disabled(!keep)
    .sub(!keep ? '服务就绪后可以切换' : keep.wakelock ? '持有中：息屏时 CPU 与 Wi-Fi 保持唤醒' : '已释放：息屏后可能延迟收发');
  openRow.disabled(!S.installed);
  restartRow.disabled(!S.installed || !d?.enabled || S.restarting);
  diag.set(diagnostics());
}

async function probe() {
  refreshBtn.classList.add('spin');
  const r = await ctl('probe');
  const p = readProbe(r);
  refreshBtn.classList.remove('spin');
  up.sync(p);
  render();
}

const refresh = poll(probe, 4000);
render();
refresh();
