/* S31 音乐播放器 - 前端逻辑
 *  - 与 web 服务器：memory 读写 / lx-server 搜索代理
 *  - 与开发板：WebSocket 直连（ws://<esp-ip>:8080/ws）
 *  - 歌单与播放模式由本端持有；播完/失败后自动下发下一首（一首一发协议）
 */
"use strict";

/* ---------------- 全局状态 ---------------- */
let mem = null;               // memory.json 内容
let ws = null;                // WebSocket 实例
let wsWantClose = false;      // 用户主动断开标记
let connected = false;
let curSong = null;           // 当前播放曲目 meta {id,title,artist,cover,duration}
let curState = "idle";        // idle / preparing / playing / paused
let searchCache = [];         // 最近一次搜索结果
let lyricTimer = null;        // 歌词同步定时器
let lyrics = [];              // 解析后的 LRC [{t, text}]

const $ = (id) => document.getElementById(id);

/* ---------------- 工具 ---------------- */
function fmtTime(sec) {
  sec = Math.max(0, Math.floor(sec));
  return Math.floor(sec / 60) + ":" + String(sec % 60).padStart(2, "0");
}
function toast(msg, isErr = false) {
  const t = $("toast");
  t.textContent = msg;
  t.style.background = isErr ? "#6e2b2b" : "#2c3347";
  t.classList.add("show");
  clearTimeout(t._timer);
  t._timer = setTimeout(() => t.classList.remove("show"), 2500);
}
function logLine(text, cls = "info") {
  const el = $("log");
  const time = new Date().toLocaleTimeString();
  const div = document.createElement("div");
  div.className = cls;
  div.textContent = `[${time}] ${text}`;
  el.appendChild(div);
  while (el.childElementCount > 200) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}
async function api(path) {
  const r = await fetch(path);
  return r.json();
}
async function saveMemory(patch) {
  Object.assign(mem, patch);
  await fetch("/api/memory", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(patch),
  });
}

/* ---------------- WebSocket 连接 ---------------- */
function connectWS(ip) {
  if (ws) { wsWantClose = true; ws.close(); }
  wsWantClose = false;
  setConn(false, "连接中...");
  ws = new WebSocket(`ws://${ip}:8080/ws`);

  ws.onopen = () => {
    connected = true;
    setConn(true, ip);
    logLine(`已连接到开发板 ${ip}`);
    // IDF master 握手不调用 handler，先发 hello 让 s31 记录 fd 并同步状态
    ws.send(JSON.stringify({ cmd: "hello" }));
  };
  ws.onclose = () => {
    connected = false;
    setConn(false, "未连接");
    if (!wsWantClose) {
      logLine("连接断开，3 秒后重连...", "err");
      setTimeout(() => { if (!connected && !wsWantClose) connectWS(ip); }, 3000);
    }
  };
  ws.onerror = () => { /* onclose 会跟着触发 */ };
  ws.onmessage = (e) => handleEvent(e.data);
}

function setConn(on, text) {
  $("conn-dot").className = "dot " + (on ? "on" : "off");
  $("conn-text").textContent = text;
}

function wsSend(obj) {
  if (!connected) { toast("未连接开发板", true); return false; }
  ws.send(JSON.stringify(obj));
  return true;
}

/* ---------------- s31 事件处理 ---------------- */
/* 按 id 找曲目 meta（搜索缓存 → 歌单 → lx-server 详情） */
async function findMeta(id) {
  let m = searchCache.find((s) => s.id === id) || mem.playlist.find((s) => s.id === id);
  if (m) return m;
  try {
    const r = await api("/api/lx/song?id=" + encodeURIComponent(id));
    if (r.song && r.song.id) return r.song;
  } catch { /* 忽略 */ }
  return { id, title: id, artist: "--" };
}

function handleEvent(raw) {
  let m;
  try { m = JSON.parse(raw); } catch { return; }
  switch (m.evt) {
    case "hello":
      $("volume").value = m.volume;
      $("volume-num").value = m.volume;
      logLine(`同步状态: 音量 ${m.volume}%, 状态 ${m.state}`);
      break;
    case "state":
      curState = m.state;
      if (m.state === "playing" && curSong && m.id && m.id !== curSong.id) {
        /* 自动换源成功：实际播放编号与请求不同，同步显示 */
        findMeta(m.id).then((meta) => {
          curSong = meta;
          $("cur-title").textContent = meta.title || meta.id;
          $("cur-artist").textContent = meta.artist || "--";
          if (meta.cover) $("cover").src = "/api/cover?u=" + encodeURIComponent(meta.cover);
          $("t-dur").textContent = fmtTime(meta.duration || 0);
          logLine(`自动换源播放: ${meta.title} (${meta.id})`);
        });
      }
      if (m.state === "idle" && !curSong) $("btn-playpause").textContent = "▶";
      break;
    case "finished":
      logLine(`播放完成: ${songName(m.id)}`);
      playNextInPlaylist(m.id);      // 依歌单+模式自动下发下一首
      break;
    case "error":
      logLine(`播放失败 [${m.id}]: ${m.msg}`, "err");
      toast(`播放失败: ${songName(m.id)}（${m.msg}）`, true);
      /* 失败自动跳下一首：仅当当前歌曲来自歌单（手动搜歌失败不劫持播放） */
      if (mem.playlist.some((s) => s.id === m.id || (curSong && s.id === curSong.id))) {
        playNextInPlaylist(m.id);
      }
      break;
    case "log":
      logLine(m.text);
      break;
    case "progress":
      updateProgress(m.pos, m.dur);
      syncLyric(m.pos);
      break;
    case "volume":
      $("volume").value = m.value;
      $("volume-num").value = m.value;
      break;
    case "sys": {
      $("cpu-chip").textContent = `CPU ${m.cpu >= 0 ? m.cpu + "%" : "--"} · 堆 ${Math.round(m.heap / 1024)}KB · RSSI ${m.rssi}dBm`;
      break;
    }
    case "pong":
      break;
    default:
      logLine("收到: " + raw);
  }
}

function songName(id) {
  if (curSong && curSong.id === id) return `${curSong.title} - ${curSong.artist}`;
  const inList = mem.playlist.find((s) => s.id === id);
  return inList ? `${inList.title} - ${inList.artist}` : id;
}

/* ---------------- 播放控制 ---------------- */

/* 音源可靠性等级：酷我(直连稳定) > 咪咕 > 酷狗(上游波动大) */
function srcRank(id) {
  if (id.startsWith("kw_")) return 0;
  if (id.startsWith("mg_")) return 1;
  if (id.startsWith("kg_")) return 2;
  return 9;
}
function srcName(id) {
  if (id.startsWith("kw_")) return "酷我";
  if (id.startsWith("mg_")) return "咪咕";
  if (id.startsWith("kg_")) return "酷狗";
  return "其他";
}
/* 归一化标题：去掉括号后缀（Live/DJ/版式说明）便于跨源匹配同一首歌 */
function normTitle(t) {
  return (t || "").replace(/[（(【\[].*$/, "").trim();
}

/* 收集同一首歌的备选源编号（跨源同名同歌手，可靠性排序，≤3 个） */
function buildAlt(meta) {
  const pool = [...searchCache, ...mem.playlist];
  const base = normTitle(meta.title);
  const artist = (meta.artist || "").slice(0, 4);
  const alts = pool
    .filter((s) => s.id !== meta.id
      && normTitle(s.title) === base
      && (artist === "" || (s.artist || "").includes(artist) || meta.artist.includes((s.artist || "").slice(0, 4))))
    .sort((a, b) => srcRank(a.id) - srcRank(b.id))
    .map((s) => s.id);
  return [...new Set(alts)].slice(0, 3);
}

function playSong(meta, altList) {
  curSong = meta;
  $("cur-title").textContent = meta.title || meta.id;
  $("cur-artist").textContent = meta.artist || "--";
  const cover = $("cover");
  if (meta.cover) cover.src = "/api/cover?u=" + encodeURIComponent(meta.cover);
  else cover.removeAttribute("src");
  $("progress").value = 0;
  $("t-cur").textContent = "0:00";
  $("t-dur").textContent = fmtTime(meta.duration || 0);
  const cmd = { cmd: "play", id: meta.id };
  const alts = altList || buildAlt(meta);
  if (alts.length) cmd.alt = alts;
  if (wsSend(cmd)) {
    $("btn-playpause").textContent = "⏸";
    logLine(`发送播放: ${meta.title} (${meta.id})` +
            (alts.length ? `，备选 ${alts.map((x) => srcName(x)).join("/")}` : ""));
    loadLyric(meta.id);
  }
}

function playNextInPlaylist(finishedId) {
  const list = mem.playlist;
  if (!list.length) { $("btn-playpause").textContent = "▶"; return; }
  let idx = list.findIndex((s) => s.id === finishedId);
  if (mem.mode === "one" && idx >= 0) {
    playSong(list[idx]);
    return;
  }
  if (mem.mode === "random") {
    let r;
    do { r = Math.floor(Math.random() * list.length); } while (list.length > 1 && r === idx);
    playSong(list[r]);
    return;
  }
  // 顺序：当前在歌单里则下一首，否则从第一首开始
  if (idx >= 0 && idx + 1 < list.length) playSong(list[idx + 1]);
  else playSong(list[0]);
}

$("btn-playpause").onclick = () => {
  if (curState === "playing") { wsSend({ cmd: "pause" }); $("btn-playpause").textContent = "▶"; }
  else if (curState === "paused") { wsSend({ cmd: "resume" }); $("btn-playpause").textContent = "⏸"; }
  else if (mem.playlist.length) { playSong(mem.playlist[0]); }
  else toast("歌单为空，请先添加歌曲", true);
};
$("btn-stop").onclick = () => { wsSend({ cmd: "stop" }); $("btn-playpause").textContent = "▶"; };
$("btn-next").onclick = () => {
  const list = mem.playlist;
  if (!list.length) return toast("歌单为空", true);
  let idx = list.findIndex((s) => curSong && s.id === curSong.id);
  if (mem.mode === "random") return playNextInPlaylist(curSong ? curSong.id : "");
  playSong(list[(idx + 1 + list.length) % list.length]);
};
$("btn-prev").onclick = () => {
  const list = mem.playlist;
  if (!list.length) return toast("歌单为空", true);
  let idx = list.findIndex((s) => curSong && s.id === curSong.id);
  if (mem.mode === "random") return playNextInPlaylist(curSong ? curSong.id : "");
  playSong(list[(idx - 1 + list.length) % list.length]);
};

/* 音量（防抖 200ms，通知 s31 + 存 memory） */
let volTimer = null;
function applyVolume(v) {
  v = Math.max(0, Math.min(100, v | 0));
  $("volume").value = v;
  $("volume-num").value = v;
  clearTimeout(volTimer);
  volTimer = setTimeout(() => {
    wsSend({ cmd: "volume", value: v });
    saveMemory({ volume: v });
  }, 200);
}
$("volume").oninput = (e) => applyVolume(parseInt(e.target.value));
$("volume-num").onchange = (e) => applyVolume(parseInt(e.target.value) || 0);

/* 播放模式 */
$("mode").onchange = (e) => {
  mem.mode = e.target.value;
  saveMemory({ mode: mem.mode });
  toast("播放模式: " + e.target.selectedOptions[0].text);
};

/* ---------------- 搜索 ---------------- */
async function doSearch() {
  const q = $("search-input").value.trim();
  if (!q) return;
  $("search-results").innerHTML = `<div class="empty">搜索中...</div>`;
  try {
    const r = await api("/api/lx/search?query=" + encodeURIComponent(q));
    searchCache = r.songs || [];
    renderSearch();
    if (!searchCache.length) toast("没有找到 MP3 音源", true);
  } catch (e) {
    $("search-results").innerHTML = `<div class="empty">搜索失败: ${e}</div>`;
  }
}
$("btn-search").onclick = doSearch;
$("search-input").addEventListener("keydown", (e) => { if (e.key === "Enter") doSearch(); });

function renderSearch() {
  const el = $("search-results");
  if (!searchCache.length) { el.innerHTML = `<div class="empty">无结果</div>`; return; }
  el.innerHTML = "";
  // 音源可靠性排序：酷我 > 咪咕 > 酷狗
  const sorted = [...searchCache].sort((a, b) => srcRank(a.id) - srcRank(b.id));
  for (const s of sorted) {
    const row = document.createElement("div");
    row.className = "row";
    row.innerHTML = `
      <span class="src-badge src-${s.id.slice(0, 2)}">${srcName(s.id)}</span>
      <div class="info"><div class="t"></div><div class="a"></div></div>
      <button data-act="play">▶ 播放</button>
      <button data-act="add">+ 歌单</button>`;
    row.querySelector(".t").textContent = s.title;
    row.querySelector(".a").textContent = `${s.artist} · ${fmtTime(s.duration)} · ${s.bitrate}k`;
    row.querySelector('[data-act="play"]').onclick = () => playSong(s);
    row.querySelector('[data-act="add"]').onclick = () => addPlaylist(s);
    el.appendChild(row);
  }
}

/* ---------------- 歌单 ---------------- */
function addPlaylist(s) {
  if (mem.playlist.some((x) => x.id === s.id)) return toast("已在歌单中");
  mem.playlist.push(s);
  saveMemory({ playlist: mem.playlist });
  renderPlaylist();
  toast(`已添加: ${s.title}`);
}
function removePlaylist(id) {
  mem.playlist = mem.playlist.filter((s) => s.id !== id);
  saveMemory({ playlist: mem.playlist });
  renderPlaylist();
}
$("btn-clear").onclick = () => {
  mem.playlist = [];
  saveMemory({ playlist: mem.playlist });
  renderPlaylist();
};
function renderPlaylist() {
  const el = $("playlist");
  if (!mem.playlist.length) { el.innerHTML = `<div class="empty">歌单为空（搜索后点击“+ 歌单”添加）</div>`; return; }
  el.innerHTML = "";
  mem.playlist.forEach((s, i) => {
    const row = document.createElement("div");
    row.className = "row" + (curSong && curSong.id === s.id ? " playing" : "");
    row.innerHTML = `
      <span class="idx">${i + 1}.</span>
      <div class="info"><div class="t"></div><div class="a"></div></div>
      <button data-act="play">▶</button>
      <button data-act="del">✕</button>`;
    row.querySelector(".t").textContent = s.title;
    row.querySelector(".a").textContent = `${s.artist} · ${fmtTime(s.duration)}`;
    row.querySelector('[data-act="play"]').onclick = () => playSong(s);
    row.querySelector('[data-act="del"]').onclick = () => removePlaylist(s.id);
    el.appendChild(row);
  });
}

/* ---------------- 进度与歌词 ---------------- */
function updateProgress(posMs, durMs) {
  if (durMs > 0) $("progress").value = Math.round((posMs / durMs) * 1000);
  $("t-cur").textContent = fmtTime(posMs / 1000);
  if (durMs > 0) $("t-dur").textContent = fmtTime(durMs / 1000);
}

async function loadLyric(songId) {
  lyrics = [];
  clearInterval(lyricTimer);
  try {
    const r = await api("/api/lx/lyric?id=" + encodeURIComponent(songId));
    if (r.lyric && r.lyric.includes("[")) {
      lyrics = parseLrc(r.lyric);
      logLine(`歌词加载: ${lyrics.length} 行`);
    }
  } catch { /* 歌词可选，忽略 */ }
}
function parseLrc(text) {
  const out = [];
  for (const line of text.split("\n")) {
    const m = line.match(/\[(\d+):(\d+)(?:\.(\d+))?\](.*)/);
    if (m) {
      out.push({ t: parseInt(m[1]) * 60 + parseInt(m[2]) + (m[3] ? parseInt(m[3]) / 100 : 0), text: m[4].trim() });
    }
  }
  return out.sort((a, b) => a.t - b.t);
}
function syncLyric(posMs) {
  if (!lyrics.length) return;
  const t = posMs / 1000;
  let cur = "";
  for (const l of lyrics) { if (l.t <= t) cur = l.text; else break; }
  if (cur && $("cur-artist").dataset.lyric !== cur) {
    $("cur-artist").dataset.lyric = cur;
    $("cur-artist").textContent = cur;   // 歌手行临时显示当前歌词（允许秒级误差）
    clearTimeout(syncLyric._t);
    syncLyric._t = setTimeout(() => {
      $("cur-artist").textContent = curSong ? curSong.artist : "--";
    }, 4000);
  }
}

/* ---------------- IP 连接弹窗 ---------------- */
function showIpModal() {
  $("ip-input").value = mem.esp_ip || "";
  $("ip-modal").classList.remove("hidden");
  if (!mem.esp_ip) $("ip-input").focus();
}
$("btn-ipcfg").onclick = showIpModal;
$("btn-ip-cancel").onclick = () => $("ip-modal").classList.add("hidden");
$("btn-ip-ok").onclick = () => {
  const ip = $("ip-input").value.trim();
  if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(ip)) return toast("IP 格式不正确", true);
  mem.esp_ip = ip;
  saveMemory({ esp_ip: ip });
  $("ip-modal").classList.add("hidden");
  logLine(`连接 ${ip} ...`);
  connectWS(ip);
};
$("ip-input").addEventListener("keydown", (e) => { if (e.key === "Enter") $("btn-ip-ok").click(); });

/* ---------------- 初始化 ---------------- */
(async function init() {
  mem = await api("/api/memory");
  $("volume").value = mem.volume;
  $("volume-num").value = mem.volume;
  $("mode").value = mem.mode;
  renderPlaylist();
  logLine("web 页面已加载");
  // 初次使用无 IP 时自动弹窗；有 IP 也弹窗（需点击确认才连接）
  showIpModal();
})();
