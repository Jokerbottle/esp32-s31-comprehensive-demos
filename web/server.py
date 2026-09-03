# -*- coding: utf-8 -*-
"""
web/server.py - 音乐播放器 web 服务器

职责（纯 Python 标准库，无需安装任何依赖）：
  1. 托管 ./static 下的前端页面（浏览器打开 http://<本机IP>:8888）；
  2. memory.json 记忆文件读写（ESP IP / 音量 / 歌单 / 播放模式 / lx-server 账号）；
  3. lx-server API 代理（规避浏览器跨域）：搜索(MP3 过滤) / 歌曲详情 / 歌词 / 封面图。

说明：浏览器与 ESP32-S31 的 WebSocket 直连（ws://<esp-ip>:8080/ws），不经过本服务器。
"""
import json
import os
import re
import socket
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(BASE_DIR, "static")
MEMORY_PATH = os.path.join(BASE_DIR, "memory.json")
PORT = 8888

DEFAULT_MEMORY = {
    "esp_ip": "",
    "volume": 40,
    "mode": "seq",          # seq=顺序 / one=单曲循环 / random=随机
    "playlist": [],          # [{id,title,artist,album,duration,cover}]
    "lx_base": "http://192.168.1.100:9527",
    "lx_user": "lxserver",
    "lx_pass": "changeme",
}


def load_memory():
    if not os.path.exists(MEMORY_PATH):
        save_memory(DEFAULT_MEMORY)
    with open(MEMORY_PATH, "r", encoding="utf-8") as f:
        mem = json.load(f)
    # 补齐缺失字段
    for k, v in DEFAULT_MEMORY.items():
        mem.setdefault(k, v)
    return mem


def save_memory(mem):
    with open(MEMORY_PATH, "w", encoding="utf-8") as f:
        json.dump(mem, f, ensure_ascii=False, indent=2)


def http_get_text(url, timeout=15):
    req = urllib.request.Request(url, headers={"User-Agent": "s31-web/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


# ---------------- lx-server Subsonic 代理 ----------------

def lx_subsonic_url(mem, path, extra):
    q = urllib.parse.urlencode(extra)
    return f"{mem['lx_base']}/rest/{path}?u={mem['lx_user']}&p={mem['lx_pass']}&v=1.16.1&c=web&{q}"


def parse_song_attrs(seg):
    """从 <song .../> 片段提取常用属性"""
    def attr(name):
        m = re.search(name + r'="([^"]*)"', seg)
        return m.group(1) if m else ""
    return {
        "id": attr("id"),
        "title": attr("title"),
        "artist": attr("artist"),
        "album": attr("album"),
        "duration": int(attr("duration") or 0),
        "cover": attr("coverArt"),
        "bitrate": attr("bitRate"),
        "suffix": attr("suffix"),
        "contentType": attr("contentType"),
    }


def lx_search(mem, query):
    """search3 搜索，仅返回 MP3（contentType=audio/mpeg），按音源可靠性排序
    （kw_ 酷我直连最稳 > mg_ 咪咕 > kg_ 酷狗上游波动大），保证 s31 播放成功率"""
    url = lx_subsonic_url(mem, "search3.view", {"query": query, "songCount": 50})
    raw = http_get_text(url).decode("utf-8", "replace")
    songs = []
    idx = 0
    while True:
        i = raw.find("<song ", idx)
        if i < 0:
            break
        j = raw.find("/>", i)
        seg = raw[i:j]
        if 'contentType="audio/mpeg"' in seg:
            s = parse_song_attrs(seg)
            if s["id"]:
                songs.append(s)
        idx = j
    rank = {"kw": 0, "mg": 1, "kg": 2}
    songs.sort(key=lambda s: rank.get(s["id"][:2], 9))
    return songs


def lx_song(mem, song_id):
    """getSong 查询单曲详情（含封面/时长），用于播放展示"""
    url = lx_subsonic_url(mem, "getSong.view", {"id": song_id})
    raw = http_get_text(url).decode("utf-8", "replace")
    i = raw.find("<song ")
    if i < 0:
        return None
    j = raw.find("/>", i)
    return parse_song_attrs(raw[i:j])


def lx_lyric(mem, song_id):
    """getLyric 歌词（可能为 LRC 格式）"""
    url = lx_subsonic_url(mem, "getLyric.view", {"id": song_id})
    raw = http_get_text(url).decode("utf-8", "replace")
    m = re.search(r"<lyric[^>]*>(.*?)</lyric>", raw, re.S)
    if not m:
        return {"lyric": ""}
    import html
    return {"lyric": html.unescape(m.group(1)).strip()}


# ---------------- HTTP Handler ----------------

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj, ensure_ascii=False))

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)
        try:
            if path == "/" or path == "/index.html":
                return self._static("index.html", "text/html; charset=utf-8")
            if path in ("/app.js", "/app.css"):
                return self._static(path.lstrip("/"))
            if path == "/api/memory":
                return self._json(load_memory())
            if path == "/api/lx/search":
                mem = load_memory()
                query = qs.get("query", [""])[0].strip()
                if not query:
                    return self._json({"songs": []})
                return self._json({"songs": lx_search(mem, query)})
            if path == "/api/lx/song":
                mem = load_memory()
                song_id = qs.get("id", [""])[0].strip()
                return self._json({"song": lx_song(mem, song_id)})
            if path == "/api/lx/lyric":
                mem = load_memory()
                song_id = qs.get("id", [""])[0].strip()
                return self._json(lx_lyric(mem, song_id))
            if path == "/api/cover":
                u = qs.get("u", [""])[0]
                if not u.startswith(("http://", "https://")):
                    return self._send(400, "bad url", "text/plain")
                data = http_get_text(u, timeout=10)
                ctype = "image/jpeg"
                if u.lower().endswith(".png") or data[:8] == b"\x89PNG\r\n\x1a\n":
                    ctype = "image/png"
                return self._send(200, data, ctype)
            return self._send(404, "not found", "text/plain")
        except Exception as e:  # noqa
            return self._json({"error": str(e)}, 502)

    def do_POST(self):
        if self.path != "/api/memory":
            return self._send(404, "not found", "text/plain")
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length).decode("utf-8")
            patch = json.loads(body)
            mem = load_memory()
            # 只允许覆盖白名单字段
            for k in DEFAULT_MEMORY:
                if k in patch:
                    mem[k] = patch[k]
            save_memory(mem)
            return self._json({"ok": True})
        except Exception as e:  # noqa
            return self._json({"error": str(e)}, 400)

    def _static(self, name, ctype=None):
        fp = os.path.join(STATIC_DIR, name)
        if not os.path.exists(fp):
            return self._send(404, "not found", "text/plain")
        if ctype is None:
            ctype = {
                ".js": "application/javascript; charset=utf-8",
                ".css": "text/css; charset=utf-8",
            }.get(os.path.splitext(name)[1], "application/octet-stream")
        with open(fp, "rb") as f:
            self._send(200, f.read(), ctype)

    def log_message(self, fmt, *args):  # 静默访问日志
        pass


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.1.1", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


if __name__ == "__main__":
    load_memory()  # 首次运行生成默认 memory.json
    ip = lan_ip()
    print(f"=== 音乐播放器 web 服务器 ===")
    print(f"浏览器打开: http://{ip}:{PORT}")
    print(f"记忆文件  : {MEMORY_PATH}")
    print(f"按 Ctrl+C 退出")
    try:
        ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\n已退出")
