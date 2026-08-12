#!/usr/bin/env python3
"""架构图生成/精修工具（gpt-image，经 OpenAI 兼容中继）。

密钥与中继地址均从环境读取，不写入仓库：
  OPENAI_API_KEY     必填，请使用轮换后的密钥
  OPENAI_BASE_URL    默认 https://sub.rongkaitech.com/v1
  OPENAI_IMAGE_MODEL 默认 gpt-image-2

约定：架构/机制图的全部文字标签由模型原生渲染，随后经查看→批注→
edits 原地精修循环校正拼写，不再由外部叠加平铺文字。

用法：
  gptgen.py generate --prompt-file P.txt --out figures/out/Fx.png [--size 1536x1024]
  gptgen.py edit --in figures/out/Fx.png --prompt "把左下角标签改为 A55" --out figures/out/Fx.png
"""
import argparse
import base64
import json
import os
import sys
import urllib.request
import uuid

BASE = os.environ.get("OPENAI_BASE_URL", "https://sub.rongkaitech.com/v1").rstrip("/")
KEY = os.environ.get("OPENAI_API_KEY")
MODEL = os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2")


def _require_key():
    if not KEY:
        sys.exit(
            "OPENAI_API_KEY 未设置。请在 figures/.env（已被 gitignore）中写入轮换后的密钥后再运行。"
        )


def _save(resp_bytes, out):
    obj = json.loads(resp_bytes)
    d = obj["data"][0]
    if d.get("b64_json"):
        data = base64.b64decode(d["b64_json"])
    elif d.get("url"):
        data = urllib.request.urlopen(d["url"], timeout=300).read()
    else:
        sys.exit("响应中无图像：" + resp_bytes[:300].decode("utf-8", "replace"))
    with open(out, "wb") as f:
        f.write(data)
    print("wrote", out, len(data), "bytes")


def generate(prompt, out, size):
    _require_key()
    body = json.dumps({"model": MODEL, "prompt": prompt, "size": size, "n": 1}).encode()
    req = urllib.request.Request(
        BASE + "/images/generations",
        data=body,
        headers={"Authorization": "Bearer " + KEY, "Content-Type": "application/json"},
    )
    _save(urllib.request.urlopen(req, timeout=300).read(), out)


def edit(inp, prompt, out, size):
    _require_key()
    b = uuid.uuid4().hex
    parts = []

    def field(name, value):
        parts.append(("--" + b).encode())
        parts.append(('Content-Disposition: form-data; name="%s"' % name).encode())
        parts.append(b"")
        parts.append(value.encode())

    with open(inp, "rb") as f:
        img = f.read()
    parts.append(("--" + b).encode())
    parts.append(
        b'Content-Disposition: form-data; name="image"; filename="img.png"'
    )
    parts.append(b"Content-Type: image/png")
    parts.append(b"")
    parts.append(img)
    field("model", MODEL)
    field("prompt", prompt)
    field("size", size)
    field("n", "1")
    parts.append(("--" + b + "--").encode())
    parts.append(b"")
    payload = b"\r\n".join(parts)
    req = urllib.request.Request(
        BASE + "/images/edits",
        data=payload,
        headers={
            "Authorization": "Bearer " + KEY,
            "Content-Type": "multipart/form-data; boundary=" + b,
        },
    )
    _save(urllib.request.urlopen(req, timeout=300).read(), out)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--prompt")
    g.add_argument("--prompt-file")
    g.add_argument("--out", required=True)
    g.add_argument("--size", default="1536x1024")
    e = sub.add_parser("edit")
    e.add_argument("--in", dest="inp", required=True)
    e.add_argument("--prompt", required=True)
    e.add_argument("--out", required=True)
    e.add_argument("--size", default="1536x1024")
    a = ap.parse_args()
    if a.cmd == "generate":
        prompt = a.prompt
        if a.prompt_file:
            with open(a.prompt_file, encoding="utf-8") as f:
                prompt = f.read()
        if not prompt:
            sys.exit("需要 --prompt 或 --prompt-file")
        generate(prompt, a.out, a.size)
    else:
        edit(a.inp, a.prompt, a.out, a.size)


if __name__ == "__main__":
    main()
