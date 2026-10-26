#!/usr/bin/env python3
"""Mevcut gen/all shard'larina OVERRIDE uygula (tam yeniden uretim olmadan).

genall.py'nin OVERRIDE=... ile uretecegi ciktinin aynisi: secilen
fonksiyonun tanimi `sub_X_orig` (ve MEMARG=1 icin `sub_X_orig_m` ve
stubs.c icinde `sub_X_m` koprusu) olarak guncellenir, decls.h'e
bildirimleri eklenir.  Etkilenen shard adlarini basar; yalnizca onlari
yeniden derlemek yeter:

    .venv/bin/python tools/override_patch.py 66ace2,6691b1
    -> shard_0085 shard_0101 ...

Zaten _orig olanlar atlanir (tekrar calistirmak guvenli).
Tam yeniden uretimde ayni adresler genall.py OVERRIDE listesine girmeli.
"""
import sys, os, re, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'gen', 'all')

def main():
    if len(sys.argv) < 2:
        print(f"kullanim: {sys.argv[0]} <hex_addr>[,<hex_addr>...]", file=sys.stderr)
        sys.exit(1)
    addrs = [int(x, 16) for x in sys.argv[1].replace('0x', '').split(',') if x.strip()]
    if not addrs:
        return

    found = {}
    for path in sorted(glob.glob(os.path.join(GEN, 'shard_*.c'))):
        with open(path, 'r', encoding='utf-8') as f:
            src = f.read()
        changed = False
        for a in addrs:
            # MEMARG=1 case
            pat_m = f"void sub_{a:08x}_m(CPU *restrict c, uint8_t *restrict _mb) {{"
            pat_w = f"void sub_{a:08x}(CPU *c) {{ sub_{a:08x}_m(c, _gm()); }}"
            if pat_m in src:
                src = src.replace(pat_m, f"void sub_{a:08x}_orig_m(CPU *restrict c, uint8_t *restrict _mb) {{", 1)
                src = src.replace(pat_w, f"void sub_{a:08x}_orig(CPU *c) {{ sub_{a:08x}_orig_m(c, _gm()); }}", 1)
                found[a] = os.path.basename(path)[:-2]
                changed = True
            else:
                # Non-MEMARG case
                pat_plain = f"void sub_{a:08x}(CPU *restrict c) {{"
                if pat_plain in src:
                    src = src.replace(pat_plain, f"void sub_{a:08x}_orig(CPU *restrict c) {{", 1)
                    found[a] = os.path.basename(path)[:-2]
                    changed = True
                elif f"void sub_{a:08x}_orig" in src:
                    found[a] = os.path.basename(path)[:-2]
        if changed:
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(src)

    # stubs.c
    stubs_path = os.path.join(GEN, 'stubs.c')
    if os.path.exists(stubs_path):
        with open(stubs_path, 'r', encoding='utf-8') as f:
            stubs = f.read()
        existing_bridges = set()
        for m in re.finditer(r'void sub_([0-9a-f]{8})_m\(CPU \*restrict c, uint8_t \*restrict _mb\)\{\s*\(void\)_mb;\s*sub_[0-9a-f]{8}\(c\);\s*\}', stubs):
            existing_bridges.add(int(m.group(1), 16))

        if existing_bridges:
            all_bridges = sorted(existing_bridges.union(addrs))
            first_m = re.search(r'void sub_[0-9a-f]{8}_m\(CPU \*restrict c, uint8_t \*restrict _mb\)', stubs)
            if first_m:
                prefix = stubs[:first_m.start()]
                bridge_lines = [f"void sub_{a:08x}_m(CPU *restrict c, uint8_t *restrict _mb){{ (void)_mb; sub_{a:08x}(c); }}\n" for a in all_bridges]
                with open(stubs_path, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(prefix + "".join(bridge_lines))

    # decls.h
    decls_path = os.path.join(GEN, 'decls.h')
    if os.path.exists(decls_path):
        with open(decls_path, 'r', encoding='utf-8') as f:
            d = f.read()

        orig_matches = list(re.finditer(r'void sub_([0-9a-f]{8})_orig\(CPU\*\);', d))
        if orig_matches:
            existing_orig = {int(m.group(1), 16) for m in orig_matches}
            all_orig = sorted(existing_orig.union(addrs))
            orig_block = "".join(f"void sub_{a:08x}_orig(CPU*);\n" for a in all_orig)
            start_pos = orig_matches[0].start()
            end_pos = orig_matches[-1].end()
            if end_pos < len(d) and d[end_pos] == '\n':
                end_pos += 1
            d = d[:start_pos] + orig_block + d[end_pos:]
        else:
            add = [f"void sub_{a:08x}_orig(CPU*);\n" for a in addrs if f"void sub_{a:08x}_orig(CPU*);" not in d]
            if add:
                d = d.replace("#endif\n", "".join(add) + "#endif\n")

        orig_m_matches = list(re.finditer(r'void sub_([0-9a-f]{8})_orig_m\(CPU \*restrict, uint8_t \*restrict\);', d))
        if orig_m_matches:
            existing_orig_m = {int(m.group(1), 16) for m in orig_m_matches}
            all_orig_m = sorted(existing_orig_m.union(addrs))
            orig_m_block = "".join(f"void sub_{a:08x}_orig_m(CPU *restrict, uint8_t *restrict);\n" for a in all_orig_m)
            start_pos = orig_m_matches[0].start()
            end_pos = orig_m_matches[-1].end()
            if end_pos < len(d) and d[end_pos] == '\n':
                end_pos += 1
            d = d[:start_pos] + orig_m_block + d[end_pos:]

        with open(decls_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(d)

    for a in addrs:
        if a not in found:
            print(f"  (atlandi / zaten override: {a:08x})", file=sys.stderr)
    print(" ".join(sorted(set(found.values()))))

if __name__ == '__main__':
    main()
