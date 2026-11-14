(function () {
  'use strict';
  /* ODAC client portal. Talks only to /cgi-bin/odac?action=...; JSON POST with
   * CSRF token from status. Session token lives in an HttpOnly cookie set by the
   * CGI; nothing is persisted client-side except the theme preference. */
  var API = '/cgi-bin/odac?action=';
  var csrf = null;
  var $ = function (id) { return document.getElementById(id); };

  /* Errors that reveal anything about an account's authorization state are
   * reported identically, so the portal never discloses which accounts exist
   * or what state they are in. A wrong password stays a wrong password. */
  var NO_PERMISSION = '无权限，请联系网络管理员';
  var ERR = {
    LOGIN_FAILED: '用户名或密码错误', NO_ACCESS: NO_PERMISSION,
    DEVICE_BLOCKED: NO_PERMISSION, DEVICE_LIMIT: NO_PERMISSION,
    SESSION_LIMIT: NO_PERMISSION,
    REGISTRATION_FAILED: NO_PERMISSION, DEVICE_REGISTERED: NO_PERMISSION,
    CAPACITY: NO_PERMISSION,
    ENFORCEMENT_FAILED: NO_PERMISSION, ENFORCEMENT_PENDING: NO_PERMISSION,
    PORTAL_DISABLED: '当前网络未启用认证，无需登录即可访问',
    CSRF: '页面已过期，已刷新，请重试',
    RATE_LIMIT: '尝试过于频繁，请稍后再试',
    INVALID_INPUT: '输入不符合要求，请检查后重试',
    PEER: '无法识别本设备，请确认已连接认证网络', HTTPS_REQUIRED: '必须通过 HTTPS 访问门户才能提交密码',
    DATABASE: '认证服务暂不可用，请稍后再试', CRYPTO_FAILED: '认证服务暂不可用，请稍后再试',
    METHOD: '请求无效', SESSION: '会话已过期，请重新登录'
  };

  /* Never surface a raw backend message: unknown codes get a generic line so
   * the portal cannot leak internal detail by accident. */
  function msg(e) {
    if (!e) return '请求失败';
    if (e.code && ERR[e.code]) return ERR[e.code];
    if (e.code === 'NETWORK') return '无法连接认证服务器，请检查网络连接';
    if (e.code === 'BAD_RESPONSE') return '服务器响应无效，请稍后再试';
    if (e.code === 'TIMEOUT') return '请求超时，请检查网络连接';
    return '请求失败，请稍后再试';
  }

  /**
   * 发送API请求，支持30秒超时
   * @param {string} action - 操作名称（status/login/register/logout等）
   * @param {object} [body] - 请求体（POST时需要）
   * @returns {Promise} 返回响应数据或抛出错误
   */
  function req(action, body) {
    var opt = { method: body ? 'POST' : 'GET', credentials: 'same-origin', cache: 'no-store', headers: { Accept: 'application/json' } };
    if (body) { opt.headers['Content-Type'] = 'application/json'; opt.body = JSON.stringify(body); }

    // Keep the deadline active until the response body has been consumed.
    var controller = new AbortController();
    var timeout = setTimeout(function () { controller.abort(); }, 30000);
    opt.signal = controller.signal;

    return fetch(API + encodeURIComponent(action), opt).then(function (r) {
      return r.json().catch(function (err) {
        if (err && err.name === 'AbortError') throw err;
        throw { code: 'BAD_RESPONSE', message: '服务器响应无效（HTTP ' + r.status + '）' };
      });
    }, function (err) {
      if (err && err.name === 'AbortError') throw err;
      throw { code: 'NETWORK', message: '无法连接认证服务器，请检查网络连接' };
    })
      .then(function (j) {
        if (j && j.ok === true) return j.data || {};
        throw (j && j.error) || { code: 'BAD_RESPONSE', message: '服务器响应无效' };
      })
      .then(function (data) { clearTimeout(timeout); return data; }, function (err) {
        clearTimeout(timeout);
        if (controller.signal.aborted) throw { code: 'TIMEOUT', message: '请求超时' };
        throw err;
      });
  }

  function set(node, text, cls) {
    node.textContent = '';
    if (cls) { var s = document.createElement('span'); s.className = 'pill ' + cls; s.textContent = text; node.appendChild(s); }
    else node.textContent = (text == null || text === '') ? '—' : String(text);
  }

  function banner(text, cls) {
    var b = $('banner');
    if (!text) { b.hidden = true; return; }
    b.hidden = false; b.className = 'banner ' + cls; b.textContent = text;
  }

  var TEXT = { portal_title: 't-title', portal_subtitle: 't-subtitle', portal_register_hint: 't-register-hint', portal_footer: 't-footer' };
  var builtin = {};
  Object.keys(TEXT).forEach(function (k) { builtin[k] = $(TEXT[k]).textContent; });

  /**
   * 安全读取localStorage，忽略权限错误
   * @param {string} key - 键名
   * @param {string} [defaultValue=''] - 默认值
   * @returns {string} 存储值或默认值
   */
  function getLocalStorage(key, defaultValue) {
    try { return localStorage.getItem(key) || (defaultValue || ''); } catch (e) { return defaultValue || ''; }
  }

  /**
   * 安全写入localStorage，忽略权限错误
   * @param {string} key - 键名
   * @param {string} value - 值
   */
  function setLocalStorage(key, value) {
    try { localStorage.setItem(key, value); } catch (e) {}
  }

  function texts(t) {
    Object.keys(TEXT).forEach(function (k) {
      var v = t && typeof t[k] === 'string' ? t[k].trim() : '';
      $(TEXT[k]).textContent = v || builtin[k];
    });
    document.title = $('t-title').textContent;
  }

  function fmt(ts) { if (ts == null || ts === '') return ''; var d = new Date(ts * 1000); return isNaN(d) ? '' : d.toLocaleString('zh-CN', { hour12: false }); }
  function secure() { return location.protocol === 'https:'; }

  var authed = false;
  function render(d) {
    csrf = d.csrf || null;
    texts(d.text);
    var dev = d.device || null, s = d.session || null;
    var open = d.portal_enabled === false;
    if (dev) set($('st-dev'), [dev.ip || dev.ipv4, dev.mac].filter(Boolean).join(' · ') + (dev.policy === 'WHITELIST' ? '（免登录）' : dev.policy === 'BLACKLIST' ? '（已禁止）' : ''));
    else set($('st-dev'), '未识别');
    authed = !!(s && (s.active || s.username));
    if (authed) set($('st-sess'), '已登录 ' + (s.username || '') + (s.expires_at ? '，有效至 ' + fmt(s.expires_at) : ''), 'ok');
    else set($('st-sess'), open ? '无需登录' : '未登录', open ? 'ok' : 'muted');
    $('logout').hidden = !authed;
    var whitelisted = !!(dev && dev.policy === 'WHITELIST');
    if (open) { $('auth-card').hidden = true; $('done-card').hidden = true; }
    else if (authed || whitelisted) { $('auth-card').hidden = true; $('done-card').hidden = true; }
    else if ($('done-card').hidden) $('auth-card').hidden = false;

    if (!secure()) banner('当前连接未加密，登录和注册仅能在 HTTPS 门户页面进行。', 'warn');
    else if (dev && dev.policy === 'BLACKLIST') banner('本设备已被管理员禁止接入。', 'bad');
    else if (open) banner('当前网络未启用认证，无需登录即可访问。', 'ok');
    else if (authed) banner('已登录，可以访问互联网。', 'ok');
    else if (whitelisted) banner('本设备已获准免登录接入。', 'ok');
    else banner(null);
  }

  function refresh() {
    return req('status').then(render, function (e) { csrf = null; banner(msg(e), 'bad'); });
  }

  function busy(form, on) { var b = form.querySelector('button[type=submit]'); b.disabled = on; b.setAttribute('aria-busy', on ? 'true' : 'false'); }
  function ferr(form, t) { form.querySelector('.err').textContent = t || ''; }

  function needCsrf() {
    if (csrf) return Promise.resolve();
    return refresh().then(function () { if (!csrf) throw { code: 'CSRF_MISSING', message: '无法获取安全令牌，请刷新页面' }; });
  }

  function submit(form, action, body, onOk) {
    ferr(form, '');
    if (!secure()) { ferr(form, ERR.HTTPS_REQUIRED); return; }
    busy(form, true);
    needCsrf().then(function () { body.csrf = csrf; return req(action, body); })
      .then(onOk, function (e) {
        ferr(form, msg(e));
        if (e && e.code === 'CSRF') csrf = null;
        return refresh();
      })
      .then(function () { busy(form, false); }, function () { busy(form, false); });
  }

  function val(f, n) { return f.elements[n].value; }

  $('p-login').addEventListener('submit', function (ev) {
    ev.preventDefault(); var f = ev.target;
    var u = val(f, 'username').trim(), p = val(f, 'password');
    if (!u || !p) return ferr(f, '请输入用户名和密码');
    submit(f, 'login', { username: u, password: p }, function () {
      f.elements.password.value = '';
      return refresh();
    });
  });

  $('p-reg').addEventListener('submit', function (ev) {
    ev.preventDefault(); var f = ev.target;
    var u = val(f, 'username').trim(), p = val(f, 'password'), c = val(f, 'confirm_password');
    if (!/^[A-Za-z0-9_.-]{3,32}$/.test(u)) return ferr(f, '用户名需为 3–32 位字母、数字或 _ . -');
    if (p.length < 6) return ferr(f, '密码至少 6 位');
    if (p !== c) return ferr(f, '两次输入的密码不一致');
    submit(f, 'register', { username: u, password: p, confirm_password: c }, function () {
      f.reset();
      $('auth-card').hidden = true; $('done-card').hidden = false; $('done-back').focus();
    });
  });

  $('done-back').addEventListener('click', function () {
    $('done-card').hidden = true; $('auth-card').hidden = false; select('tab-login');
    $('p-login').elements.username.focus();
  });

  $('logout').addEventListener('click', function () {
    var b = this; b.disabled = true;
    needCsrf().then(function () { return req('logout', { csrf: csrf }); })
      .then(function () { banner('已退出登录。', 'ok'); }, function (e) { banner('退出失败：' + msg(e), 'bad'); })
      .then(refresh).then(function () { b.disabled = false; });
  });

  $('refresh').addEventListener('click', refresh);

  var tabs = ['tab-login', 'tab-reg'];
  function select(id) {
    tabs.forEach(function (t) {
      var on = t === id, b = $(t);
      b.setAttribute('aria-selected', on); b.tabIndex = on ? 0 : -1;
      $(b.getAttribute('aria-controls')).hidden = !on;
    });
  }
  tabs.forEach(function (t, i) {
    $(t).addEventListener('click', function () { select(t); });
    $(t).addEventListener('keydown', function (e) {
      var d = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
      if (d) { var n = tabs[(i + d + tabs.length) % tabs.length]; select(n); $(n).focus(); }
    });
  });

  var modes = ['system', 'light', 'dark'], LBL = { system: '跟随系统', light: '亮色', dark: '暗色' };
  /**
   * 设置并应用主题
   * @param {string} m - 主题模式 ('system'|'light'|'dark')
   */
  function theme(m) {
    if (m === 'system') document.documentElement.removeAttribute('data-theme'); else document.documentElement.setAttribute('data-theme', m);
    $('theme').setAttribute('aria-label', '主题：' + LBL[m] + '（点击切换）'); $('theme').title = LBL[m];
  }
  var cur = getLocalStorage('odac-theme', 'system');
  if (modes.indexOf(cur) < 0) cur = 'system';
  theme(cur);
  $('theme').addEventListener('click', function () {
    cur = modes[(modes.indexOf(cur) + 1) % modes.length]; theme(cur);
    setLocalStorage('odac-theme', cur);
  });

  refresh();
})();
