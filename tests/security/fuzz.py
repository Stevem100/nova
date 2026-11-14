#!/usr/bin/env python3
"""fuzz.py — differential fuzzer for the Nova interpreter.

Generates malformed Nova sources and malformed JSON documents, feeds them
through the interpreter (ideally the ASan/UBSan build: `make debug`) and
reports any sanitizer crash, hang or signal death.

Usage:
    python3 tests/security/fuzz.py --nova ./nova_debug --count 400
    python3 tests/security/fuzz.py --nova ./nova_debug --count 400 --save DIR
                                                       (keep crashing inputs)
"""
import argparse
import json
import os
import random
import subprocess
import sys
import tempfile

SEED = 20261114

# fragments the mutation engine recombines
SNIPPETS = [
    'echo 1;', 'echo "s";', '$x = 1;', 'fn f($a) { return $a; }',
    'if ($x) { }', 'while (true) { break; }', 'foreach ([1,2] as $v) { }',
    'switch ($x) { case 1: echo "a"; break; default: break; }',
    'try { throw "e"; } catch ($e) { } finally { }',
    'class C { fn m() { return 1; } } $c = new C(); echo $c->m();',
    '$m = ["a" => 1]; echo $m->a;', 'echo $arr[0];', 'echo f(1);',
    'echo 1 + 2 * 3;', 'echo "a" . "b";', 'echo -1;', 'echo !true;',
    'echo 2 ** 3;', 'echo $x ?? "d";', 'echo $x ? 1 : 2;',
    'echo len([1,2,3]);', 'echo json_encode(["k" => 1]);',
    'echo sha256("x");', 'echo base64_encode("x");',
    'import "strx";', 'echo $strx->slug("a b");',
]

BAD_BITS = [
    '"', "'", '(', ')', '[', ']', '{', '}', '$', '\\', '\x00', '\xff',
    '/*', '*/', '??', '->', '=>', '**', '...', ';', '#', '`', '\r',
    '9' * 40, 'e' * 40, '\\u', '\\x', chr(0x2764), chr(0x10FFFF),
]

JSON_TEMPLATES = [
    '{"a": [1, 2, {"b": "x"}], "c": null}',
    '[1, -2.5e3, "s", true, false, null]',
    '"plain string"',
    '{"nested": {"deep": {"deeper": [1, 2, 3]}}}',
    '{"unicode": "\\u0041\\u00e9"}',
]

JSON_BAD = [
    '\\u', '"\\', ',', '{:', '["a" "b"]', '{"x"}', '1.2.3', '-',
    'truX', 'nil', "'sq'", '\\uDC00\\uD800', '\x00', '}', '{',
    ':', '0x1f', '+5', '.5', '5.', 'NaN', 'Infinity', '"\\q"',
]


def mutate(s, rnd, k):
    for _ in range(k):
        p = rnd.randint(0, len(s))
        op = rnd.random()
        if op < 0.45:
            s = s[:p] + rnd.choice(BAD_BITS) + s[p:]
        elif op < 0.75 and s:
            p2 = min(len(s), p + rnd.randint(1, 6))
            s = s[:p] + s[p2:]
        else:
            s = s[:p] + rnd.choice(s) if s else s
    return s


def build_sources(rnd, n):
    out = []
    for _ in range(n):
        parts = [rnd.choice(SNIPPETS) for _ in range(rnd.randint(1, 6))]
        src = "\n".join(parts)
        src = mutate(src, rnd, rnd.randint(0, 8))
        if rnd.random() < 0.25:                      # structural bombs
            k = rnd.choice([50, 300, 3000, 60000])
            bomb = rnd.choice([
                "(" * k + "1" + ")" * k,
                "-" * k + "1",
                "[" * k + "]" * k,
                "{",
                "echo " + "1 ?? " * min(k, 20000) + "1;",
                '"' + "a" * (k * 16) + ("\\x" if rnd.random() < 0.5 else ""),
            ])
            src = bomb + "\n" + src if rnd.random() < 0.5 else bomb
        out.append(src)
    return out


def build_json(rnd, n):
    out = []
    for _ in range(n):
        s = rnd.choice(JSON_TEMPLATES)
        s = mutate(s, rnd, rnd.randint(1, 6))
        out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nova", default="./nova_debug")
    ap.add_argument("--count", type=int, default=400)
    ap.add_argument("--save", default=None, help="dir to keep crashers")
    args = ap.parse_args()

    rnd = random.Random(SEED)
    sources = build_sources(rnd, args.count // 2)
    jsons = build_json(rnd, args.count - args.count // 2)

    crashes = 0
    timeouts = 0
    tmpdir = tempfile.mkdtemp(prefix="novafuzz")

    for i, src in enumerate(sources):
        path = os.path.join(tmpdir, "f%05d.nv" % i)
        with open(path, "w", encoding="utf-8", errors="surrogatepass") as f:
            f.write(src)
        try:
            r = subprocess.run([args.nova, "run", path], capture_output=True,
                               text=True, timeout=15,
                               errors="replace")
            rc = r.returncode
            err = (r.stderr or "").lower()
        except subprocess.TimeoutExpired:
            timeouts += 1
            continue
        if rc < 0 or "sanitizer" in err or "deadlysignal" in err:
            crashes += 1
            print("CRASH(rc=%s): %s" % (rc, src[:120].replace("\n", "\\n")))
            if args.save:
                os.makedirs(args.save, exist_ok=True)
                with open(os.path.join(args.save, "crash_%05d.nv" % i), "w") as f:
                    f.write(src)
        os.unlink(path)

    for i, doc in enumerate(jsons):
        code = "json_decode(%s);" % json.dumps(doc)
        try:
            r = subprocess.run([args.nova, "-e", code], capture_output=True,
                               text=True, timeout=15, errors="replace")
            rc = r.returncode
            err = (r.stderr or "").lower()
        except subprocess.TimeoutExpired:
            timeouts += 1
            continue
        if rc < 0 or "sanitizer" in err or "deadlysignal" in err:
            crashes += 1
            print("CRASH(rc=%s): %s" % (rc, doc[:120]))
            if args.save:
                os.makedirs(args.save, exist_ok=True)
                with open(os.path.join(args.save, "crash_json_%05d.txt" % i), "w") as f:
                    f.write(doc)

    print("fuzz: %d sources + %d JSON docs -> %d crashes, %d timeouts"
          % (len(sources), len(jsons), crashes, timeouts))
    if crashes or timeouts:
        print("FAIL: interpreter is not crash-free under fuzzing")
        sys.exit(1)
    print("PASS: crash-free")
    sys.exit(0)


if __name__ == "__main__":
    main()
