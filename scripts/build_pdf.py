# -*- coding: utf-8 -*-
"""
把 docs/parts/*.md 合并成一份完整文档，生成 HTML，再用无头 Edge/Chrome 打印成 PDF。

用法:
    python scripts/build_pdf.py

产物:
    docs/CppLearningGuide.md     合并后的 Markdown（可直接在编辑器里读）
    docs/CppLearningGuide.html   带样式和目录的 HTML（可直接用浏览器打开）
    docs/CppLearningGuide.pdf    打印版 PDF
"""
import io
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARTS_DIR = os.path.join(ROOT, "docs", "parts")
OUT_DIR = os.path.join(ROOT, "docs")
BASENAME = "CppLearningGuide"

CSS = r"""
:root{
  --fg:#1f2328; --fg-dim:#57606a; --bg:#ffffff;
  --border:#d0d7de; --accent:#0969da; --code-bg:#f6f8fa;
  --note-bg:#fff8c5; --note-bd:#d4a72c;
}
*{box-sizing:border-box}
html{font-size:15px}
body{
  margin:0 auto; padding:2.2rem 2.6rem 4rem; max-width:60rem;
  color:var(--fg); background:var(--bg);
  font-family:"Segoe UI","Microsoft YaHei","PingFang SC","Hiragino Sans GB",
              "Source Han Sans SC","Noto Sans CJK SC",sans-serif;
  line-height:1.75; word-wrap:break-word;
}
h1,h2,h3,h4{line-height:1.3; margin:2em 0 .7em; font-weight:650}
h1{font-size:1.95rem; border-bottom:2px solid var(--border); padding-bottom:.4rem;
   page-break-before:auto}
h2{font-size:1.45rem; border-bottom:1px solid var(--border); padding-bottom:.3rem}
h3{font-size:1.18rem}
h4{font-size:1.02rem; color:var(--fg-dim)}
h1:first-child{margin-top:0}
p{margin:.75em 0}
a{color:var(--accent); text-decoration:none}
a:hover{text-decoration:underline}
hr{border:none; border-top:1px solid var(--border); margin:2.2em 0}

code{
  font-family:"Cascadia Mono","Consolas","SF Mono","JetBrains Mono",
              "Microsoft YaHei Mono",monospace;
  font-size:.875em; background:var(--code-bg);
  padding:.15em .38em; border-radius:5px;
}
pre{
  background:var(--code-bg); border:1px solid var(--border); border-radius:7px;
  padding:.85rem 1rem; overflow-x:auto; line-height:1.55;
  page-break-inside:avoid;
}
pre code{background:none; padding:0; font-size:.82rem; white-space:pre}

blockquote{
  margin:1.1em 0; padding:.6em 1em; border-left:4px solid var(--note-bd);
  background:var(--note-bg); border-radius:0 6px 6px 0;
}
blockquote p{margin:.3em 0}

table{
  border-collapse:collapse; width:100%; margin:1.1em 0;
  font-size:.9rem; page-break-inside:avoid;
}
th,td{border:1px solid var(--border); padding:.45em .7em; text-align:left;
      vertical-align:top}
th{background:var(--code-bg); font-weight:650}
tr:nth-child(even) td{background:#fbfcfd}

ul,ol{padding-left:1.6em; margin:.7em 0}
li{margin:.28em 0}
li>ul,li>ol{margin:.2em 0}

/* Pygments 代码高亮 */
.codehilite .k,.codehilite .kd,.codehilite .kt,.codehilite .kr{color:#cf222e}
.codehilite .kc,.codehilite .kn,.codehilite .kp{color:#cf222e}
.codehilite .s,.codehilite .s1,.codehilite .s2,.codehilite .sc{color:#0a3069}
.codehilite .c,.codehilite .c1,.codehilite .cm,.codehilite .cp,
.codehilite .cs{color:#6e7781; font-style:italic}
.codehilite .n{color:#1f2328}
.codehilite .nf,.codehilite .nc{color:#8250df}
.codehilite .nb{color:#0550ae}
.codehilite .mi,.codehilite .mf,.codehilite .mh{color:#0550ae}
.codehilite .o,.codehilite .p{color:#1f2328}
.codehilite .err{color:#1f2328; border:none}

#doc-toc{
  border:1px solid var(--border); border-radius:8px;
  padding:1rem 1.4rem; margin:2rem 0; background:#fafbfc;
  page-break-after:always;
}
#doc-toc>p{font-weight:650; font-size:1.15rem; margin:.2em 0 .6em}
#doc-toc ul{list-style:none; padding-left:1em}
#doc-toc>ul{padding-left:0}
#doc-toc li{margin:.18em 0; font-size:.92rem}
#doc-toc a{color:var(--fg)}

.cover{text-align:center; padding:4rem 0 2rem}
.cover h1{border:none; font-size:2.6rem; margin:.2em 0}
.cover .sub{color:var(--fg-dim); font-size:1.05rem; margin:.4em 0}
.cover .meta{color:var(--fg-dim); font-size:.9rem; margin-top:2.5rem}

@page{ size:A4; margin:15mm 14mm; }
@media print{
  html{font-size:10.5pt}
  body{max-width:none; padding:0}
  pre,table,blockquote{page-break-inside:avoid}
  h1,h2,h3{page-break-after:avoid}
  a{color:var(--fg); text-decoration:none}
}
"""


def collect_parts():
    if not os.path.isdir(PARTS_DIR):
        sys.exit("找不到 docs/parts 目录")
    files = sorted(f for f in os.listdir(PARTS_DIR) if f.endswith(".md"))
    if not files:
        sys.exit("docs/parts 下没有 .md 文件")
    chunks = []
    for f in files:
        with io.open(os.path.join(PARTS_DIR, f), encoding="utf-8") as fh:
            chunks.append(fh.read().rstrip())
    return files, "\n\n".join(chunks) + "\n"


def to_html(md_text):
    try:
        import markdown
    except ImportError:
        sys.exit("缺少依赖，请先执行:  python -m pip install markdown pygments")

    exts = ["fenced_code", "tables", "toc", "attr_list", "sane_lists", "md_in_html"]
    cfg = {"toc": {"permalink": False, "toc_depth": "1-3"}}
    try:
        import pygments  # noqa: F401
        exts.append("codehilite")
        cfg["codehilite"] = {"guess_lang": False, "css_class": "codehilite"}
    except ImportError:
        print("  [提示] 没装 pygments，代码不高亮（pip install pygments 可开启）")

    md = markdown.Markdown(extensions=exts, extension_configs=cfg)
    body = md.convert(md_text)
    toc = getattr(md, "toc", "")

    cover = (
        '<div class="cover">'
        "<h1>C++ 复习与 Linux 网络编程</h1>"
        '<p class="sub">完整学习手册 · 从语法回忆到手写 HTTP 服务器</p>'
        '<p class="sub">配套工程：CppLearning（15 个可运行程序）</p>'
        '<p class="meta">生成时间 %s</p>'
        "</div>" % time.strftime("%Y-%m-%d %H:%M")
    )

    toc_block = ""
    if toc:
        toc_block = '<nav id="doc-toc"><p>目录</p>%s</nav>' % re.sub(
            r'^<div class="toc">|</div>$', "", toc.strip()
        )

    return (
        "<!doctype html>\n<html lang=\"zh-CN\">\n<head>\n"
        '<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
        "<title>C++ 复习与 Linux 网络编程</title>\n"
        "<style>%s</style>\n</head>\n<body>\n%s\n%s\n%s\n</body>\n</html>\n"
        % (CSS, cover, toc_block, body)
    )


def find_browser():
    candidates = [
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    for name in ("msedge", "chrome", "chromium", "google-chrome"):
        p = shutil.which(name)
        if p:
            return p
    return None


def html_to_pdf(html_path, pdf_path):
    browser = find_browser()
    if not browser:
        print("  [跳过 PDF] 没找到 Edge / Chrome。")
        print("            可以手动用浏览器打开 HTML，Ctrl+P -> 另存为 PDF。")
        return False

    if os.path.exists(pdf_path):
        try:
            os.remove(pdf_path)
        except OSError:
            print("  [警告] 旧 PDF 被占用（是不是在阅读器里开着？），请关闭后重试")
            return False

    url = "file:///" + html_path.replace("\\", "/")
    profile = os.path.join(ROOT, "docs", ".pdfprofile")
    cmd = [
        browser,
        "--headless=new",
        "--disable-gpu",
        "--no-sandbox",
        "--no-pdf-header-footer",
        "--run-all-compositor-stages-before-draw",
        "--virtual-time-budget=10000",
        "--user-data-dir=" + profile,
        "--print-to-pdf=" + pdf_path,
        url,
    ]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=180)

    # 有些版本 --headless=new 不写文件，退回老的 headless
    if not os.path.exists(pdf_path):
        cmd[1] = "--headless"
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)

    shutil.rmtree(profile, ignore_errors=True)
    return os.path.exists(pdf_path)


def main():
    files, md_text = collect_parts()
    print("合并 %d 个片段:" % len(files))
    for f in files:
        print("   -", f)

    md_path = os.path.join(OUT_DIR, BASENAME + ".md")
    with io.open(md_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(md_text)
    print("\n[1/3] Markdown -> %s  (%.0f KB)" % (md_path, len(md_text.encode()) / 1024))

    html = to_html(md_text)
    html_path = os.path.join(OUT_DIR, BASENAME + ".html")
    with io.open(html_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(html)
    print("[2/3] HTML     -> %s  (%.0f KB)" % (html_path, len(html.encode()) / 1024))

    pdf_path = os.path.join(OUT_DIR, BASENAME + ".pdf")
    if html_to_pdf(html_path, pdf_path):
        print("[3/3] PDF      -> %s  (%.0f KB)"
              % (pdf_path, os.path.getsize(pdf_path) / 1024))
    else:
        print("[3/3] PDF      -> 生成失败")


if __name__ == "__main__":
    main()
