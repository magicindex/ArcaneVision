#!/usr/bin/env python3
"""UI 交互静态回归：鼠标打开策略与视频播放器接线。

不做浏览器渲染，只校验 index.html 里关键选择器、默认值和事件绑定仍在；
内联脚本若本机有 node，会再做一次语法检查，避免手工编辑引入 JS 语法错误。
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTML = os.path.join(ROOT, "ui", "index.html")
src = open(HTML, encoding="utf-8").read()

def must(cond, name):
    if not cond:
        print(f"FAIL {name}")
        sys.exit(1)

# 鼠标打开方式：默认文件夹单击、文件双击；设置项存在
must('id="scanCard"' in src and 'id="scanMode"' in src, "scan mode setting")
must('id="scanStatus"' in src and 'id="scanRollback"' in src, "scan status/rollback")
must('id="scanCandidates"' in src and 'id="scanRefresh"' in src, "scan candidate selection")
must('scanmode/candidates' in src, "candidate API")
must("API + 'scanmode'" in src, "scanmode API wiring")
must('id="clickFolder"' in src, "clickFolder setting")
must('id="clickFile"' in src, "clickFile setting")
must("clickFolder: 'single'" in src and "clickFile: 'double'" in src, "click defaults")
must("function clickModeOf(it)" in src, "clickModeOf helper")
must("if (clickModeOf(it) === 'single')" in src, "single-click branch")
must("if (clickModeOf(it) === 'double') openEntry(it, items);" in src, "double-click branch")
must("if (coarse) { openEntry(it, items); return; }" in src, "touch single-tap")
must("clickFile: 'double'" in src, "file default double")

# 视频播放器：自定义控制条、离开隐藏、快捷键
must('id="videoview"' in src, "video view")
must('id="vvideo"' in src, "video element")
must('id="vseek"' in src and 'id="vplay"' in src, "video controls")
video_tag = re.search(r"<video[^>]*>", src)
must(video_tag is not None, "video tag")
must("controls" not in video_tag.group(0), "custom controls (no native controls attr)")
must("videoView.addEventListener('mouseleave'" in src, "mouseleave hide")
must("videoHideControls" in src, "hide controls helper")
must("e.key === 'ArrowLeft'" in src and "videoSeekBy(-5)" in src, "left 5s")
must("e.key === 'ArrowRight'" in src and "videoSeekBy(5)" in src, "right 5s")
must("e.code === 'Space'" in src and "videoToggle()" in src, "space toggle")
must("Math.min(max, cur + delta)" in src, "seek clamp")
must('id="searchCard"' in src and 'id="searchMulti"' in src, "multi search setting")
must('id="searchMatchAll"' in src and 'id="searchSep"' in src, "multi search options")
must('id="searchCaseSensitive"' in src and 'id="searchOrdered"' in src, "multi search flags")
must('id="rbMode"' in src and 'idle' in src and 'rbIdleHint' in src, "idle scan setting")

# 内联脚本语法检查（可选）
scripts = re.findall(r"<script>(.*?)</script>", src, re.S)
if scripts and shutil.which("node"):
    fd, path = tempfile.mkstemp(suffix=".js")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(scripts[-1])
        r = subprocess.run(["node", "--check", path], capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL js syntax")
            print(r.stderr.strip())
            sys.exit(1)
    finally:
        os.unlink(path)

print("ui: OK")
