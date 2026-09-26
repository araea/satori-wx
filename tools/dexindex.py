#!/data/data/com.termux/files/usr/bin/python3
"""知言探针的离线 dex 索引器。

用法：
    python3 dexindex.py <base.apk> [-o index.json] [--filter 正则]

从 APK 里逐个解析 classes*.dex（纯 stdlib，不依赖任何库），产出：
  - classes：全部类描述符（Lcom/tencent/mm/...; 形式）
  - strings：匹配过滤正则的字符串池子集（默认 msg/message/sync/proto/listener/
    callback/Jni/Native/CppProxy，大小写不敏感）
  - dex：每个 dex 的类计数

libapp.$cso.so 一类的编译化 dex 不解析、只在 meta 里记一笔。

分析时与 boundary.log 交叉比对：注册自 libwechatnormsg/libapp.* 的 native 方法，
其类名（点分）转成描述符后在这里查语义线索，挑消息流候选边界。
"""
import argparse
import json
import re
import struct
import sys
import zipfile

DEFAULT_FILTER = r"msg|message|sync|proto|listener|callback|jni|native|cppproxy"


def read_uleb128(data, pos):
    result = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            return result, pos
        shift += 7


def parse_dex(data):
    """返回 (classes_descriptors, all_strings_iterable)。"""
    if data[:4] not in (b"dex\n", b"DEX\x00"):
        raise ValueError("not a dex file: %r" % data[:8])
    string_ids_size, string_ids_off = struct.unpack_from("<II", data, 0x38)
    type_ids_size, type_ids_off = struct.unpack_from("<II", data, 0x40)
    class_defs_size, class_defs_off = struct.unpack_from("<II", data, 0x60)

    # 字符串池：先记偏移，惰性解码。
    string_offsets = struct.unpack_from("<%dI" % string_ids_size, data, string_ids_off) \
        if string_ids_size else ()

    def get_string(idx):
        if idx >= len(string_offsets):
            return ""
        pos = string_offsets[idx]
        _, pos = read_uleb128(data, pos)   # utf16 长度（字符数），解码用不到
        end = data.index(b"\x00", pos)
        return data[pos:end].decode("utf-8", errors="replace")

    # 类型描述符：type_ids 指向 string_ids。
    type_desc = [get_string(i) for i in
                 struct.unpack_from("<%dI" % type_ids_size, data, type_ids_off)] \
        if type_ids_size else []

    classes = []
    for i in range(class_defs_size):
        off = class_defs_off + 32 * i
        (class_idx,) = struct.unpack_from("<I", data, off)
        if class_idx < len(type_desc):
            classes.append(type_desc[class_idx])
    return classes, string_offsets, get_string


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("apk")
    ap.add_argument("-o", "--out", default="index.json")
    ap.add_argument("--filter", default=DEFAULT_FILTER)
    args = ap.parse_args()
    pat = re.compile(args.filter, re.IGNORECASE)

    classes = []
    dex_meta = []
    filtered_strings = []
    seen_strings = set()

    with zipfile.ZipFile(args.apk) as z:
        dex_names = sorted(n for n in z.namelist()
                           if n.startswith("classes") and n.endswith(".dex"))
        if not dex_names:
            print("no classes*.dex in %s" % args.apk, file=sys.stderr)
        cso = [n for n in z.namelist() if "cso" in n.lower()]
        for name in dex_names:
            data = z.read(name)
            cs, string_offsets, get_string = parse_dex(data)
            classes.extend(cs)
            dex_meta.append({"name": name, "classes": len(cs),
                             "strings": len(string_offsets)})
            print("  %s: %d classes, %d strings" % (name, len(cs), len(string_offsets)),
                  file=sys.stderr)
            for idx, off in enumerate(string_offsets):
                pos = off
                _, pos = read_uleb128(data, pos)
                end = data.index(b"\x00", pos)
                if end - pos > 200:      # 跳过超长串
                    continue
                s = data[pos:end].decode("utf-8", errors="replace")
                if s in seen_strings or len(s) < 4:
                    continue
                if pat.search(s):
                    seen_strings.add(s)
                    filtered_strings.append(s)

    out = {
        "apk": args.apk,
        "dex": dex_meta,
        "compiled_dex_libs": cso,   # libapp.$cso.so 之类，只记录
        "class_count": len(classes),
        "classes": classes,
        "filtered_strings": filtered_strings,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    print("wrote %s: %d classes, %d filtered strings, cso=%s"
          % (args.out, len(classes), len(filtered_strings), cso), file=sys.stderr)


if __name__ == "__main__":
    main()
