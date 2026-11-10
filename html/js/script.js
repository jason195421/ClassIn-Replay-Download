/* ClassIn Replay Download - enhanced UI
 * Front-end only. Backend API is unchanged:
 *   GET  /get-titles        -> { url: [title, ...] }
 *   GET  /get-status        -> { autoscan, wait_interval }
 *   GET  /scan, /enable-autoscan, /disable-autoscan, /stop
 *   POST /set-autoscan-delay (plain text seconds)
 *   POST /require-download  -> { path, downloads:[{url,name}] }
 *   GET  /download-status   -> [{url,name,path,status,percent,msg}]
 *   DELETE /cancel-download/<index>
 * status: 1 downloading, 2 failed, 3 cancelled, 4 succeeded, 5 waiting
 */
'use strict';

/* ---------------- helpers ---------------- */
function q(sel) { return document.querySelector(sel); }
function $(id) { return document.getElementById(id); }
function HTMLEncode(s) {
    s = String(s == null ? '' : s);
    var d = document.createElement('div');
    if (d.textContent != null) d.textContent = s; else d.innerText = s;
    return d.innerHTML;
}
function escAttr(s) {
    return HTMLEncode(s).replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

/* ---------------- state ---------------- */
var videos = [];              // { id, url, titles, best, fpUrl, fpTitle, sel, player, dupRoot, dupIds, isDup, dupReason }
var byUrl = Object.create(null);
var byId = Object.create(null);
var downloads = [];
var dlEls = [];
var hiddenDl = Object.create(null);
var dupGroups = Object.create(null);
var doneUrls = Object.create(null);
var activeUrls = Object.create(null);
var lastVisible = [];
var seq = 0;
var gen = 0;
var polling = true;
var busyDelay = false;

/* ---------------- duplicate detection ----------------
 * Two independent signals are combined with a union-find:
 *  1) URL fingerprint: strip volatile query params (token/sign/expires/ts...),
 *     CDN numeric host prefixes, segment filenames and extensions. Two m3u8
 *     URLs differing only by signature/timestamp collapse to the same key.
 *  2) Title fingerprint: normalise the best window title (keep only letters,
 *     digits and CJK), drop generic words such as "classin"/"replay".
 * A bucket covering >60% of all videos is treated as a degenerate signal and
 * ignored, so a shared empty/generic title cannot merge everything.
 */
var VOLATILE = { token: 1, sign: 1, signature: 1, expires: 1, expire: 1, ts: 1, t: 1,
    timestamp: 1, nonce: 1, auth: 1, auth_key: 1, key: 1, session: 1, sid: 1, _: 1,
    r: 1, rand: 1, random: 1, us: 1, et: 1, cdn: 1, tx: 1, ws: 1, h: 1, u: 1,
    ip: 1, uid: 1, time: 1, st: 1, e: 1, cdn_type: 1, ws_secret: 1 };
var NOISE = ['classin', 'classroom', 'onlineclass', 'replay', 'playback', 'live',
    '在线教室', '回放', '直播', '播放', '观看'];

function UrlFingerprint(raw) {
    try {
        var u = new URL(raw);
        var host = (u.hostname || '').toLowerCase()
            .replace(/^(?:cdn|video|vod|v|hls|static|media|p)[-_]?\d*[-_.]/i, '')
            .replace(/^\d+[-_.]/, '');
        var path = decodeURIComponent(u.pathname || '')
            .replace(/\/(?:seg|segment|chunk|part)[-_]?\d+[^/]*/gi, '')
            .toLowerCase()
            .replace(/\.(m3u8|mp4|flv|ts|mkv|m4s|m4v)$/i, '')
            .replace(/\/+$/, '');
        var stable = [];
        u.searchParams.forEach(function (v, k) {
            var kl = k.toLowerCase();
            if (VOLATILE[kl]) return;
            if (/^\d{6,}$/.test(v)) return;
            stable.push(kl + '=' + String(v).toLowerCase());
        });
        stable.sort();
        return host + path + (stable.length ? '?' + stable.join('&') : '');
    } catch (e) {
        return String(raw).toLowerCase().split('?')[0]
            .replace(/\.(m3u8|mp4|flv|ts)$/i, '');
    }
}

function BestTitle(titles) {
    var arr = (titles || []).filter(function (t) { return !!t; });
    if (!arr.length) return '';
    var clean = arr.filter(function (t) {
        var l = t.toLowerCase();
        return l.indexOf('classin') < 0 && l.indexOf('classroom') < 0;
    });
    var pool = clean.length ? clean : arr;
    pool = pool.slice().sort(function (a, b) { return b.length - a.length; });
    return pool[0];
}

function TitleFingerprint(titles) {
    var t = BestTitle(titles);
    if (!t) return null;
    var n = t.toLowerCase().replace(/[^a-z0-9\u4e00-\u9fff]/g, '');
    NOISE.forEach(function (w) { n = n.split(w.toLowerCase()).join(''); });
    return n.length >= 6 ? n : null;
}

function RecomputeDup() {
    var n = videos.length, parent = [], i;
    for (i = 0; i < n; i++) parent.push(i);
    function find(x) { while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
    function union(a, b) { var ra = find(a), rb = find(b); if (ra !== rb) parent[rb] = ra; }
    function mergeBuckets(buckets) {
        Object.keys(buckets).forEach(function (k) {
            var g = buckets[k];
            if (g.length < 2 || g.length > n * 0.6) return;
            for (var j = 1; j < g.length; j++) union(g[0], g[j]);
        });
    }
    var bu = Object.create(null), bt = Object.create(null);
    videos.forEach(function (v, idx) {
        if (v.fpUrl && v.fpUrl.length >= 8) (bu[v.fpUrl] || (bu[v.fpUrl] = [])).push(idx);
        if (v.fpTitle) (bt[v.fpTitle] || (bt[v.fpTitle] = [])).push(idx);
    });
    mergeBuckets(bu);
    mergeBuckets(bt);

    dupGroups = Object.create(null);
    var roots = [];
    for (i = 0; i < n; i++) {
        var r = find(i);
        roots.push(r);
        (dupGroups[r] || (dupGroups[r] = [])).push(videos[i].id);
    }
    var reasonByRoot = Object.create(null);
    Object.keys(dupGroups).forEach(function (r) {
        var ids = dupGroups[r];
        if (ids.length < 2) return;
        var fu = Object.create(null), ft = Object.create(null), nu = 0, nt = 0;
        ids.forEach(function (id) {
            var v = byId[id]; if (!v) return;
            if (v.fpUrl) { fu[v.fpUrl] = 1; nu++; }
            if (v.fpTitle) { ft[v.fpTitle] = 1; nt++; }
        });
        var sameU = nu === ids.length && Object.keys(fu).length === 1;
        var sameT = nt === ids.length && Object.keys(ft).length === 1;
        reasonByRoot[r] = (sameU && sameT) ? '地址与标题均相同' :
            sameU ? '地址相同（可能仅签名或时间戳不同）' :
            sameT ? '窗口标题相同' : '地址或标题相似';
    });
    videos.forEach(function (v, idx) {
        v.dupRoot = roots[idx];
        v.dupIds = dupGroups[roots[idx]] || [v.id];
        v.isDup = v.dupIds.length > 1;
        v.dupReason = reasonByRoot[roots[idx]] || '相似';
    });
}

/* ---------------- themes & storage ---------------- */
function InitTheme() {
    var t = 'light';
    try { t = localStorage.getItem('crd_theme') || 'light'; } catch (e) {}
    document.documentElement.setAttribute('data-theme', t);
    $('theme-btn').innerHTML = t === 'dark' ? '&#9788;' : '&#9789;';
}
function ToggleTheme() {
    var cur = document.documentElement.getAttribute('data-theme');
    var next = cur === 'dark' ? 'light' : 'dark';
    document.documentElement.setAttribute('data-theme', next);
    try { localStorage.setItem('crd_theme', next); } catch (e) {}
    $('theme-btn').innerHTML = next === 'dark' ? '&#9788;' : '&#9789;';
}

/* ---------------- toast ---------------- */
function Toast(title, msg, kind) {
    var d = document.createElement('div');
    d.className = 'toast ' + (kind || 'info');
    d.innerHTML = '<b>' + HTMLEncode(title) + '</b>' + HTMLEncode(msg);
    $('toast-wrap').appendChild(d);
    setTimeout(function () {
        d.classList.add('out');
        setTimeout(function () { if (d.parentNode) d.parentNode.removeChild(d); }, 260);
    }, 3600);
}

function Offline() {
    if (!polling) return;
    polling = false;
    Toast('连接已断开', '程序可能已退出，页面不再自动刷新', 'warn');
}

/* ---------------- video cards ---------------- */
function SafeFilename(s) {
    return String(s || '')
        .replace(/[\\/:*?"<>|]/g, '')
        .replace(/[\s\u3000]+/g, ' ')
        .trim()
        .slice(0, 120);
}

function AddVideo(url, titles) {
    if (byUrl[url]) {
        var v0 = byUrl[url], changed = false;
        titles.forEach(function (t) {
            if (v0.titles.indexOf(t) < 0) { v0.titles.push(t); changed = true; }
        });
        if (changed) {
            $('list' + v0.id).innerHTML = v0.titles.map(function (t) {
                return '<option>' + HTMLEncode(t) + '</option>';
            }).join('');
            v0.best = BestTitle(v0.titles);
            $('vtitle' + v0.id).textContent = v0.best;
            v0.fpTitle = TitleFingerprint(v0.titles);
        }
        return !!changed;
    }
    var id = ++seq;
    var best = BestTitle(titles);
    var fname = SafeFilename(best) || ('replay_' + id);
    var opts = titles.map(function (t) { return '<option>' + HTMLEncode(t) + '</option>'; }).join('');

    var wrap = document.createElement('div');
    wrap.innerHTML =
        '<article class="vcard" id="item' + id + '">' +
        '  <div class="preview" id="preview' + id + '">' +
        '    <div class="poster" id="poster' + id + '" onclick="TogglePreview(' + id + ')" title="点击加载预览">' +
        '      <div class="play">&#9654;</div><div>点击加载预览</div></div>' +
        '    <div class="idx">#' + id + '</div>' +
        '    <div class="vcheck" onclick="ToggleSelect(' + id + ')" title="勾选 / 取消">&#10003;</div>' +
        '  </div>' +
        '  <div class="vbody">' +
        '    <h3 class="vtitle" id="vtitle' + id + '">' + HTMLEncode(best) + '</h3>' +
        '    <div class="tags" id="tags' + id + '"></div>' +
        '    <div class="vurl" onclick="CopyUrlById(' + id + ')" title="点击复制地址">' + HTMLEncode(url) + '</div>' +
        '    <div class="fields">' +
        '      <input type="text" id="filename' + id + '" list="list' + id + '" value="' + escAttr(fname) + '" placeholder="文件名">' +
        '      <datalist id="list' + id + '">' + opts + '</datalist>' +
        '      <select id="ext' + id + '"><option>.mp4</option><option>.mkv</option><option>.m4v</option><option>.flv</option></select>' +
        '    </div>' +
        '    <div class="vactions">' +
        '      <button class="btn primary" onclick="Download(' + id + ')">&#11015; 下载</button>' +
        '      <button class="btn" onclick="TogglePreview(' + id + ')">&#9654; 预览</button>' +
        '      <button class="btn ghost" onclick="CopyUrlById(' + id + ')" title="复制地址">&#128203;</button>' +
        '    </div>' +
        '  </div>' +
        '</article>';
    var card = wrap.firstChild;
    $('content').appendChild(card);

    var v = {
        id: id, url: url, titles: titles.slice(), best: best,
        fpUrl: UrlFingerprint(url), fpTitle: TitleFingerprint(titles),
        sel: false, player: false, playerInst: null,
        dupRoot: 0, dupIds: [id], isDup: false, dupReason: ''
    };
    videos.push(v);
    byUrl[url] = v;
    byId[id] = v;
    return true;
}

function FindVideo(id) { return byId[id]; }

function UpdateTags(v) {
    var h = '<span class="tag tag-new">' + v.titles.length + ' 个标题</span>';
    if (v.isDup) {
        h += '<span class="tag tag-dup" title="判定依据：' + escAttr(v.dupReason) +
             '" onclick="HighlightGroup(' + v.dupRoot + ')">&#9888; 疑似重复 &times;' + v.dupIds.length + '</span>';
    }
    if (doneUrls[v.url]) h += '<span class="tag tag-done">&#10003; 已下载</span>';
    else if (activeUrls[v.url]) h += '<span class="tag tag-dl">&#8635; 下载中</span>';
    var el = $('tags' + v.id);
    if (el && el.innerHTML !== h) el.innerHTML = h;
    var card = $('item' + v.id);
    if (card) card.classList.toggle('is-dup', v.isDup);
}

function ToggleSelect(id) {
    var v = FindVideo(id); if (!v) return;
    v.sel = !v.sel;
    $('item' + id).classList.toggle('selected', v.sel);
    UpdateSelCount();
    if ($('filter').value === 'selected') ApplyFilters();
}

function UpdateSelCount() {
    var n = videos.filter(function (v) { return v.sel; }).length;
    $('sel-info').textContent = '已选 ' + n + ' 项';
    $('st-selected').textContent = n;
}

function TogglePreview(id) {
    var v = FindVideo(id); if (!v) return;
    if (v.player) { try { v.playerInst.play(); } catch (e) {} return; }
    var box = $('preview' + id), poster = $('poster' + id);
    if (poster && poster.parentNode) poster.parentNode.removeChild(poster);
    var vj = document.createElement('video-js');
    vj.id = 'video' + id;
    vj.setAttribute('controls', '');
    vj.setAttribute('preload', 'none');
    vj.setAttribute('oncontextmenu', 'return false');
    vj.innerHTML = '<source src="' + escAttr(v.url) + '">';
    box.appendChild(vj);
    try {
        v.playerInst = videojs('video' + id);
        v.player = true;
    } catch (e) {
        Toast('预览不可用', '播放器初始化失败：' + (e && e.message ? e.message : e), 'warn');
        var p = document.createElement('div');
        p.className = 'poster';
        p.innerHTML = '<div>&#9654; 预览不可用</div>';
        box.appendChild(p);
    }
}

/* ---------------- filtering, sorting, grouping ---------------- */
function ApplyFilters() {
    var kw = ($('search').value || '').trim().toLowerCase();
    var filter = $('filter').value;
    var sort = $('sort').value;
    var groupDup = $('groupDup').checked;
    var hideDup = $('hideDup').checked;

    var cand = videos.filter(function (v) {
        if (kw && (v.titles.join(' ') + ' ' + v.url).toLowerCase().indexOf(kw) < 0) return false;
        if (filter === 'dup' && !v.isDup) return false;
        if (filter === 'unique' && v.isDup) return false;
        if (filter === 'downloaded' && !doneUrls[v.url] && !activeUrls[v.url]) return false;
        if (filter === 'undownloaded' && (doneUrls[v.url] || activeUrls[v.url])) return false;
        if (filter === 'selected' && !v.sel) return false;
        return true;
    });

    if (hideDup) {
        var seen = Object.create(null);
        cand = cand.filter(function (v) {
            var k = v.isDup ? 'g' + v.dupRoot : 'u' + v.id;
            if (seen[k]) return false;
            seen[k] = 1; return true;
        });
    }

    var order = Object.create(null);
    videos.forEach(function (v, i) { order[v.id] = i; });
    cand.sort(function (a, b) {
        if (groupDup) {
            var da = a.isDup ? 0 : 1, db = b.isDup ? 0 : 1;
            if (da !== db) return da - db;
            if (a.isDup && a.dupRoot !== b.dupRoot) return a.dupRoot - b.dupRoot;
        }
        if (sort === 'title') return a.best.localeCompare(b.best, 'zh-CN') || order[a.id] - order[b.id];
        if (sort === 'titles') return b.titles.length - a.titles.length || order[a.id] - order[b.id];
        if (sort === 'dup') return b.dupIds.length - a.dupIds.length || order[a.id] - order[b.id];
        return order[a.id] - order[b.id];
    });

    var visible = Object.create(null);
    cand.forEach(function (v) { visible[v.id] = 1; });
    videos.forEach(function (v) {
        var el = $('item' + v.id);
        if (el) el.hidden = !visible[v.id];
        UpdateTags(v);
    });
    cand.forEach(function (v) { $('content').appendChild($('item' + v.id)); });
    lastVisible = cand;

    var empty = $('empty');
    if (!videos.length) {
        empty.hidden = false;
        empty.querySelector('p').textContent = '还没有发现回放视频';
        empty.querySelector('span').textContent = '打开 ClassIn 并播放一段回放，然后点击「扫描回放」开始';
    } else if (!cand.length) {
        empty.hidden = false;
        empty.querySelector('p').textContent = '没有匹配的视频';
        empty.querySelector('span').textContent = '试试清空搜索或调整筛选条件';
    } else empty.hidden = true;

    var groups = Object.keys(dupGroups).filter(function (r) { return dupGroups[r].length > 1; });
    var dupVideos = 0;
    groups.forEach(function (r) { dupVideos += dupGroups[r].length; });
    $('st-total').textContent = videos.length;
    $('st-dup').textContent = groups.length;
    var banner = $('dupBanner');
    if (groups.length) {
        banner.classList.add('show');
        $('dupBannerText').textContent = '检测到 ' + groups.length + ' 组疑似重复（共 ' + dupVideos +
            ' 个视频）。批量下载前建议点击「仅选代表」避免重复下载。';
    } else banner.classList.remove('show');
    UpdateSelCount();
}

function ClearSearch() { $('search').value = ''; ApplyFilters(); $('search').focus(); }
function FocusFirstDup() {
    var v = videos.filter(function (x) { return x.isDup && !$('item' + x.id).hidden; })[0];
    if (!v) { Toast('提示', '当前没有可见的重复项', 'warn'); return; }
    HighlightGroup(v.dupRoot);
}
function HighlightGroup(root) {
    var ids = dupGroups[root] || [];
    ids.forEach(function (id) {
        var el = $('item' + id); if (!el) return;
        el.classList.add('focused');
        setTimeout(function () { el.classList.remove('focused'); }, 2200);
    });
    if (ids.length && $('item' + ids[0])) $('item' + ids[0]).scrollIntoView({ behavior: 'smooth', block: 'center' });
    Toast('重复分组', '该组共 ' + ids.length + ' 个视频，已高亮显示', 'ok');
}

/* ---------------- selection ---------------- */
function SetSelection(fn) {
    var list = lastVisible.length ? lastVisible : videos;
    list.forEach(function (v) {
        v.sel = fn(v);
        var el = $('item' + v.id);
        if (el) el.classList.toggle('selected', v.sel);
    });
    UpdateSelCount();
    if ($('filter').value === 'selected') ApplyFilters();
}
function SelectAll() { SetSelection(function () { return true; }); }
function SelectNone() { SetSelection(function () { return false; }); }
function SelectInvert() { SetSelection(function (v) { return !v.sel; }); }
function SelectUniqueOnly() {
    var reps = Object.create(null);
    videos.forEach(function (v) { if (!reps[v.dupRoot]) reps[v.dupRoot] = v.id; });
    var keep = Object.create(null);
    Object.keys(reps).forEach(function (r) { keep[reps[r]] = 1; });
    videos.forEach(function (v) {
        v.sel = !!keep[v.id];
        var el = $('item' + v.id);
        if (el) el.classList.toggle('selected', v.sel);
    });
    UpdateSelCount();
    if ($('filter').value === 'selected') ApplyFilters();
    var n = Object.keys(keep).length;
    Toast('已选代表', '保留 ' + n + ' 个代表项，跳过 ' + (videos.length - n) + ' 个疑似重复', 'ok');
}

/* ---------------- clipboard ---------------- */
function CopyText(text, okMsg) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(function () { Toast('复制成功', okMsg, 'ok'); },
            function () { LegacyCopy(text, okMsg); });
    } else LegacyCopy(text, okMsg);
}
function LegacyCopy(text, okMsg) {
    var ta = document.createElement('textarea');
    ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0';
    document.body.appendChild(ta); ta.select();
    try { document.execCommand('copy'); Toast('复制成功', okMsg, 'ok'); }
    catch (e) { Toast('复制失败', '请手动复制：' + String(text).slice(0, 60), 'err'); }
    document.body.removeChild(ta);
}
function CopyUrlById(id) {
    var v = FindVideo(id); if (!v) return;
    CopyText(v.url, '回放地址已复制到剪贴板');
}
function CopySelectedUrls() {
    var urls = videos.filter(function (v) { return v.sel; }).map(function (v) { return v.url; });
    if (!urls.length) { Toast('没有选中', '请先勾选视频', 'warn'); return; }
    CopyText(urls.join('\n'), '已复制 ' + urls.length + ' 个地址');
}

/* ---------------- download ---------------- */
function Download(id) {
    var v = FindVideo(id); if (!v) return;
    var nameEl = $('filename' + id), name = (nameEl.value || '').trim();
    if (!name) { Toast('无法下载', '请先填写文件名', 'warn'); nameEl.focus(); return; }
    if (/[\\/:*?"<>|]/.test(name)) {
        Toast('无法下载', '文件名不能包含 \\ / : * ? " < > |', 'warn'); return;
    }
    var full = name + $('ext' + id).value;
    var path = ($('path').value || '').trim();
    if (!path) Toast('注意', '下载路径为空，文件将保存到程序当前目录', 'warn');
    fetch('/require-download', {
        method: 'POST',
        body: JSON.stringify({ path: path, downloads: [{ url: v.url, name: full }] })
    }).then(function (r) { return r.text(); }).then(function (t) {
        if (t === 'success') Toast('已加入队列', full, 'ok');
        else Toast('下载被拒绝', t, 'err');
    }, function () { Toast('网络错误', '无法提交下载请求', 'err'); });
}
function DownloadSelected() {
    var sel = videos.filter(function (v) { return v.sel; });
    if (!sel.length) { Toast('没有选中', '请先勾选要下载的视频', 'warn'); return; }
    var perGroup = Object.create(null), extra = 0;
    sel.forEach(function (v) {
        if (v.isDup) perGroup[v.dupRoot] = (perGroup[v.dupRoot] || 0) + 1;
    });
    Object.keys(perGroup).forEach(function (r) { if (perGroup[r] > 1) extra += perGroup[r] - 1; });
    if (extra > 0) Toast('重复提醒', '选中项含 ' + extra + ' 个疑似重复视频，可用「仅选代表」去掉', 'warn');
    sel.forEach(function (v) { Download(v.id); });
    Toast('已提交', '共 ' + sel.length + ' 个下载任务', 'ok');
}

/* ---------------- download list ---------------- */
var STATUS_LABEL = { 1: '下载中', 2: '失败', 3: '已取消', 4: '已完成', 5: '等待中' };
function OpenDownList() { $('downlist-cover').hidden = false; RenderDownloads(); }
function CloseDownList() { $('downlist-cover').hidden = true; }
function ClearFinishedDownloads() {
    downloads.forEach(function (d, i) {
        if (d.status === 4 || d.status === 2 || d.status === 3) hiddenDl[i] = 1;
    });
    RenderDownloads();
    Toast('已清理', '已完成 / 失败的任务已从列表隐藏', 'ok');
}
function RenderDownloads() {
    var box = $('down-list');
    for (var i = dlEls.length; i < downloads.length; i++) {
        var n = document.createElement('div');
        n.className = 'list-item'; n.id = 'down-item' + i;
        n.innerHTML =
            '<div class="dl-name" id="dl-name' + i + '" title="点击复制完整路径"></div>' +
            '<div class="dl-path" id="dl-path' + i + '"></div>' +
            '<div class="dl-meta"><span class="dl-status" id="dl-st' + i + '"></span>' +
            '<span class="dl-pct" id="dl-pct' + i + '"></span>' +
            '<button class="cancel-btn" id="cancel-btn' + i + '">&times;</button></div>' +
            '<div class="progress-bar-container"><div class="progress-bar" id="dl-bar' + i + '"></div></div>' +
            '<div class="error-message" id="dl-msg' + i + '"></div>';
        (function (idx) {
            n.querySelector('#cancel-btn' + idx).onclick = function () {
                fetch('/cancel-download/' + idx, { method: 'DELETE' });
            };
            n.querySelector('#dl-name' + idx).onclick = function () {
                var d = downloads[idx];
                CopyText(d ? (d.path + '\\' + d.name) : '', '完整路径已复制');
            };
        })(i);
        box.appendChild(n);
        dlEls.push(n);
    }
    var filter = $('dlFilter').value, sum = 0, cnt = 0;
    downloads.forEach(function (d, i) {
        var el = dlEls[i]; if (!el) return;
        var show = !hiddenDl[i];
        if (filter === 'active') show = show && (d.status === 1 || d.status === 5);
        else if (filter === '4') show = show && d.status === 4;
        else if (filter === '2') show = show && (d.status === 2 || d.status === 3);
        el.hidden = !show;
        $('dl-name' + i).textContent = d.name || '(未命名)';
        $('dl-path' + i).textContent = d.path || '';
        $('dl-st' + i).textContent = STATUS_LABEL[d.status] || ('状态 ' + d.status);
        $('dl-st' + i).className = 'dl-status st-' + d.status;
        var pct = Number(d.percent) || 0;
        $('dl-pct' + i).textContent = pct.toFixed(1) + '%';
        var bar = $('dl-bar' + i);
        bar.className = 'progress-bar' + (d.status === 4 ? ' p-4' :
            (d.status === 2 || d.status === 3 ? ' p-2' : ''));
        bar.style.width = pct.toFixed(2) + '%';
        $('dl-msg' + i).textContent = d.msg || '';
        var c = $('cancel-btn' + i);
        var cancellable = (d.status === 1 || d.status === 5);
        c.className = cancellable ? 'cancel-btn' : 'cancel-btn disabled';
        c.disabled = !cancellable;
        if (show) { sum += pct; cnt++; }
    });
    var avg = cnt ? sum / cnt : 0;
    $('overall-bar').style.width = avg.toFixed(2) + '%';
    $('overall-text').textContent = avg.toFixed(0) + '%';
    var emptyEl = $('dl-empty');
    emptyEl.hidden = cnt > 0;
    emptyEl.textContent = downloads.length ? '没有符合当前筛选的任务' : '暂无下载任务';
}
function UpdateStats() {
    $('st-running').textContent = downloads.filter(function (d) { return d.status === 1 || d.status === 5; }).length;
    $('st-done').textContent = downloads.filter(function (d) { return d.status === 4; }).length;
    $('st-failed').textContent = downloads.filter(function (d) { return d.status === 2 || d.status === 3; }).length;
}

/* ---------------- polling ---------------- */
function GetVideos(myGen) {
    if (myGen === undefined) myGen = ++gen;
    if (!polling || myGen !== gen) return;
    fetch('/get-titles').then(function (r) { return r.json(); }, Offline).then(function (data) {
        if (!polling || myGen !== gen || !data || typeof data !== 'object') return;
        var changed = false;
        Object.keys(data).forEach(function (url) {
            var t = data[url];
            if (!Array.isArray(t)) t = [String(t)];
            if (byUrl[url]) {
                if (AddVideo(url, t)) changed = true;
            } else { AddVideo(url, t); changed = true; }
        });
        if (changed) { RecomputeDup(); ApplyFilters(); }
        if (polling && myGen === gen) setTimeout(function () { GetVideos(myGen); }, 1000);
    }, function () {
        if (polling && myGen === gen) setTimeout(function () { GetVideos(myGen); }, 2000);
    });
}
function RefreshVideos() {
    gen++;
    videos = []; byUrl = Object.create(null); byId = Object.create(null);
    dupGroups = Object.create(null); seq = 0; lastVisible = [];
    $('content').innerHTML = '';
    GetVideos(gen);
    Toast('已刷新', '正在重新拉取回放列表', 'ok');
}
function RefreshStatus() {
    if (!polling) return;
    fetch('/get-status').then(function (r) { return r.json(); }, Offline).then(function (d) {
        if (!polling || !d) return;
        $('autoscan').checked = !!d.autoscan;
        if (!busyDelay) $('delay').value = d.wait_interval;
        setTimeout(RefreshStatus, 1000);
    }, function () { if (polling) setTimeout(RefreshStatus, 2000); });
}
function RefreshDownloads() {
    if (!polling) return;
    fetch('/download-status').then(function (r) { return r.json(); }, Offline).then(function (data) {
        if (!polling || !Array.isArray(data)) return;
        downloads = data;
        doneUrls = Object.create(null); activeUrls = Object.create(null);
        data.forEach(function (d) {
            if (d.status === 4) doneUrls[d.url] = 1;
            if (d.status === 1 || d.status === 5) activeUrls[d.url] = 1;
        });
        RenderDownloads();
        UpdateStats();
        videos.forEach(UpdateTags);
        var running = data.filter(function (d) { return d.status === 1 || d.status === 5; }).length;
        var badge = $('dl-badge');
        badge.textContent = running;
        badge.classList.toggle('zero', running === 0);
        setTimeout(RefreshDownloads, 500);
    }, function () { if (polling) setTimeout(RefreshDownloads, 2000); });
}

/* ---------------- backend actions ---------------- */
function RequireScan() {
    fetch('/scan').then(function () { Toast('扫描已触发', '正在查找 ClassIn 窗口中的回放地址…', 'ok'); },
        function () { Toast('错误', '无法触发扫描', 'err'); });
}
function SwitchAutoScan() {
    var on = $('autoscan').checked;
    fetch(on ? '/enable-autoscan' : '/disable-autoscan');
    Toast('设置已更新', '自动扫描已' + (on ? '开启' : '关闭'), 'ok');
}
function ChangeDelayTime() {
    fetch('/set-autoscan-delay', { method: 'POST', body: $('delay').value.toString() });
}
function ExitApp() {
    if (!confirm('确定要退出下载器程序吗？')) return;
    fetch('/stop').then(function () {}, function () {});
    setTimeout(function () {
        polling = false;
        Toast('已退出', '程序已关闭，可以关闭此页面了', 'ok');
    }, 300);
}

/* ---------------- init ---------------- */
function Init() {
    InitTheme();
    try {
        var p = localStorage.getItem('crd_path');
        if (p) $('path').value = p;
    } catch (e) {}
    $('path').addEventListener('change', function () {
        try { localStorage.setItem('crd_path', this.value); } catch (e) {}
    });
    $('delay').addEventListener('focus', function () { busyDelay = true; });
    $('delay').addEventListener('blur', function () { busyDelay = false; ChangeDelayTime(); });
    document.addEventListener('keydown', function (e) {
        var tag = (e.target && e.target.tagName ? e.target.tagName : '').toLowerCase();
        var typing = tag === 'input' || tag === 'textarea' || tag === 'select';
        if (e.key === 'Escape') CloseDownList();
        if (!typing && e.key === '/') { e.preventDefault(); $('search').focus(); }
        if (!typing && (e.ctrlKey || e.metaKey) && String(e.key).toLowerCase() === 'a') {
            e.preventDefault(); SelectAll();
        }
        if (!typing && e.ctrlKey && e.key === 'Enter') DownloadSelected();
    });
    GetVideos();
    RefreshStatus();
    RefreshDownloads();
    ApplyFilters();
}
Init();
