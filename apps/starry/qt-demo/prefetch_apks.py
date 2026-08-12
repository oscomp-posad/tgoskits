#!/usr/bin/env python3
"""Resolve + download an Alpine apk dependency closure into an overlay cache.

Root packages come from the QT_DEMO_ROOTS env var (space separated).
Mirrors the resolver in apps/starry/wayland/prebuild.sh but standalone so it
can be unit-tested and reused. No signing: on-target install uses
--allow-untrusted --no-network first, then live mirrors as fallback.
"""
import io
import os
import re
import shutil
import sys
import tarfile
import urllib.request

apk_arch, branch, cache_dir, guest_cache_dir = sys.argv[1:5]
mirrors = [
    "http://mirrors.huaweicloud.com/alpine",
    "http://dl-cdn.alpinelinux.org/alpine",
    "http://mirrors.aliyun.com/alpine",
    "http://mirrors.tuna.tsinghua.edu.cn/alpine",
    "http://mirrors.cernet.edu.cn/alpine",
]
repos = ["main", "community"]
roots = os.environ.get("QT_DEMO_ROOTS", "qt6-qtbase font-dejavu fontconfig g++").split()


def dep_key(value):
    value = value.strip()
    if not value or value.startswith("!"):
        return None
    return re.split(r"[<>=~]", value, maxsplit=1)[0]


def fetch_bytes(path):
    last = None
    for mirror in mirrors:
        try:
            with urllib.request.urlopen(f"{mirror}/{branch}/{path}", timeout=120) as r:
                return r.read()
        except Exception as exc:  # noqa: BLE001
            last = exc
            print(f"warning: fetch failed {mirror}/{branch}/{path}: {exc}", file=sys.stderr)
    raise RuntimeError(f"all mirrors failed for {path}: {last}")


def fetch_file(path, target):
    last = None
    for mirror in mirrors:
        tmp = target + ".tmp"
        try:
            with urllib.request.urlopen(f"{mirror}/{branch}/{path}", timeout=180) as r, open(tmp, "wb") as out:
                shutil.copyfileobj(r, out)
            os.replace(tmp, target)
            return
        except Exception as exc:  # noqa: BLE001
            last = exc
            try:
                os.unlink(tmp)
            except FileNotFoundError:
                pass
    raise RuntimeError(f"all mirrors failed for {path}: {last}")


packages, providers = {}, {}
for repo in repos:
    data = fetch_bytes(f"{repo}/{apk_arch}/APKINDEX.tar.gz")
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as arc:
        index = arc.extractfile("APKINDEX").read().decode()
    for block in index.strip().split("\n\n"):
        fields = {}
        for line in block.splitlines():
            if len(line) > 2 and line[1] == ":":
                fields.setdefault(line[0], []).append(line[2:])
        name = fields.get("P", [None])[0]
        version = fields.get("V", [None])[0]
        if not name or not version:
            continue
        deps = []
        for d in fields.get("D", []):
            deps.extend(filter(None, (dep_key(x) for x in d.split())))
        provides = [name]
        for p in fields.get("p", []):
            provides.extend(filter(None, (dep_key(x) for x in p.split())))
        packages[name] = {"name": name, "version": version, "repo": repo, "deps": deps}
        for prov in provides:
            providers.setdefault(prov, name)

resolved, seen, queue = [], set(), list(roots)
while queue:
    req = queue.pop(0)
    name = req if req in packages else providers.get(req)
    if not name or name in seen:
        continue
    seen.add(name)
    pkg = packages[name]
    resolved.append(pkg)
    for dep in pkg["deps"]:
        dn = dep if dep in packages else providers.get(dep)
        if dn and dn not in seen:
            queue.append(dn)

os.makedirs(cache_dir, exist_ok=True)
os.makedirs(guest_cache_dir, exist_ok=True)
print(f"QT_PREFETCH resolved {len(resolved)} apk(s) for {apk_arch}")
with open(os.path.join(guest_cache_dir, "install.list"), "w", encoding="utf-8") as manifest:
    for pkg in resolved:
        filename = f"{pkg['name']}-{pkg['version']}.apk"
        cached = os.path.join(cache_dir, filename)
        if not os.path.exists(cached) or os.path.getsize(cached) == 0:
            fetch_file(f"{pkg['repo']}/{apk_arch}/{filename}", cached)
        shutil.copy2(cached, os.path.join(guest_cache_dir, filename))
        manifest.write(f"/usr/local/qt-demo-apks/{filename}\n")
print(f"QT_PREFETCH prepared {len(resolved)} apk(s)")
