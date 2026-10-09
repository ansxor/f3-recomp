"""Field relations inside RAM records: which other fields determine a target field.

Input: samples of one record layout (every active record in every dumped frame). For a target
field T the search reports, strongest first:
  - single fields F that determine T (same F value -> same T value), with a closed form when one
    fits: T == F, T == F + c, T == k*F + c, or T == ROM table[F] (element width = T's size);
  - pairs (F1, F2) that determine T;
  - residuals T - F1 determined by a field or a pair (T = F1 + g(F2[, F3])), e.g. a descriptor
    pointer = base field + class * step.
Relevant bits are reported for byte/word determinants (bits whose value changes the result).
A determinant with about one distinct key per sample proves nothing (an ID or a pointer) and is
skipped: every reported key must repeat on average (`support` = samples per key).
"""
from __future__ import annotations

import ast
import re
from dataclasses import dataclass
from itertools import combinations

MIN_SUPPORT = 4.0     # samples per distinct determinant key
MAX_KEYS = 64         # pair/residual searches only use fields with at most this many distinct values


@dataclass
class Relation:
    kind: str              # single | pair | residual
    fields: tuple          # ((off, size), ...) determinant fields; residual: (base, *determinants)
    keys: int              # distinct determinant keys
    samples: int
    form: str              # closed form or "lookup"
    mapping: dict          # key -> value (value = T, or T - base for residuals)
    bits: tuple = ()       # per determinant: mask of bits that matter


def field_name(field: tuple[int, int]) -> str:
    off, size = field
    return f"+{off:#x}.{'bwl'[{1: 0, 2: 1, 4: 2}[size]]}"


def columns(rows: list[bytes], length: int, target: tuple[int, int]) -> dict[tuple[int, int], list[int]]:
    """Every aligned byte/word/long field of the record that varies and does not overlap the target."""
    t_off, t_size = target
    cols = {}
    for size in (1, 2, 4):
        for off in range(0, length - size + 1, 1 if size == 1 else 2):
            if off < t_off + t_size and t_off < off + size:
                continue
            values = [int.from_bytes(r[off:off + size], "big") for r in rows]
            if len(set(values)) > 1:
                cols[(off, size)] = values
    return cols


def determines(keys: list, values: list[int]) -> dict | None:
    mapping: dict = {}
    for k, v in zip(keys, values):
        if mapping.setdefault(k, v) != v:
            return None
    return mapping


def closed_form(mapping: dict, size: int, rom: bytes) -> str:
    """T as a function of one field: identity, offset, linear, or a ROM table indexed by it."""
    modulus = 1 << (8 * size)
    items = sorted(mapping.items())
    if all(k % modulus == v for k, v in items):
        return "T == F (always equal here: a copy, or both written by the same code; check with f3a writes --field)"
    c = (items[0][1] - items[0][0]) % modulus
    if all((k + c) % modulus == v for k, v in items):
        return f"T == F + {c:#x}" if c < modulus // 2 else f"T == F - {modulus - c:#x}"
    (k1, v1), (k2, v2) = items[0], items[-1]
    if k2 != k1 and (v2 - v1) % (k2 - k1) == 0:
        k = (v2 - v1) // (k2 - k1)
        b = (v1 - k * k1) % modulus
        if k and all((k * key + b) % modulus == v for key, v in items):
            return f"T == {k}*F + {b:#x}"
    table = rom_table(mapping, size, rom)
    return table or "lookup"


def rom_table(mapping: dict, size: int, rom: bytes) -> str:
    """A ROM table of `size`-byte elements that reproduces mapping: T == table[F * scale]."""
    items = sorted(mapping.items())
    if len(items) < 3 or items[-1][0] - items[0][0] > 0x1000:
        return ""
    k0, v0 = items[0]
    needle = v0.to_bytes(size, "big")
    found = []
    for scale in sorted({size, 1, 2, 4}):
        start = 0
        while len(found) < 3:
            pos = rom.find(needle, start)
            if pos < 0:
                break
            start = pos + 1
            if size > 1 and pos & 1:
                continue
            base = pos - k0 * scale
            if base < 0:
                continue
            if all(base + k * scale + size <= len(rom)
                   and int.from_bytes(rom[base + k * scale:base + k * scale + size], "big") == v for k, v in items):
                found.append((base, scale))
        if found:
            break
    if not found:
        return ""
    base, scale = found[0]
    more = f" (also {', '.join(f'{b:#08x}' for b, _ in found[1:])})" if len(found) > 1 else ""
    return f"T == ROM[{base:#08x} + F*{scale}] ({size}-byte table){more}"


def search(rows: list[bytes], length: int, target: tuple[int, int], rom: bytes, limit: int = 12) -> tuple[list[Relation], dict]:
    """Relations for the target field across sample records (rows are record bytes)."""
    t_off, t_size = target
    modulus = 1 << (8 * t_size)
    rows = list(dict.fromkeys(rows))  # identical records add no evidence
    t_values = [int.from_bytes(r[t_off:t_off + t_size], "big") for r in rows]
    info = {"samples": len(rows), "distinct": len(set(t_values))}
    if info["distinct"] < 2:
        return [], info
    cols = columns(rows, length, target)
    n = len(rows)
    found: list[Relation] = []

    def support_ok(keys: list) -> int:
        count = len(set(keys))
        return count if count >= 2 and n / count >= MIN_SUPPORT else 0

    def covers(big: tuple, small_field: tuple) -> bool:
        return big[0] <= small_field[0] and small_field[0] + small_field[1] <= big[0] + big[1]

    def redundant(fields: tuple, known: list[tuple]) -> bool:
        """A determinant that contains a smaller known determinant (member-wise) adds nothing."""
        return any(len(k) <= len(fields) and all(any(covers(f, s) for f in fields) for s in k) for k in known)

    def relation(kind: str, fields: tuple, determinant: tuple, values_t: list[int], form_prefix: str = "T") -> Relation:
        masks, mapping = reduce_keys([cols[f] for f in determinant], [8 * f[1] for f in determinant], values_t)
        form = closed_form(mapping, t_size, rom).replace("T ==", f"{form_prefix} ==") if len(determinant) == 1 \
            else "lookup"
        return Relation(kind, fields, len(mapping), n, form, mapping, masks)

    known: list[tuple] = []
    # Single determinants (bytes first: the smallest field that works wins).
    for field, values in cols.items():
        if not support_ok(values) or redundant((field,), known) or determines(values, t_values) is None:
            continue
        found.append(relation("single", (field,), (field,), t_values))
        known.append((field,))
    small = [f for f, v in cols.items() if len(set(v)) <= MAX_KEYS]

    def pair_relations(values_t: list[int], exclude: set, seen: list[tuple]) -> list[tuple]:
        out = []
        for f1, f2 in combinations([f for f in small if f not in exclude], 2):
            if redundant((f1, f2), seen) or covers(f1, f2) or covers(f2, f1) \
                    or any(len(k) == 1 and covers(k[0], f1) and covers(k[0], f2) for k in seen):
                continue
            keys = list(zip(cols[f1], cols[f2]))
            if support_ok(keys) and determines(keys, values_t) is not None:
                out.append((len(set(keys)), f1, f2))
                seen.append((f1, f2))
        return sorted(out)

    if len(found) < limit:
        for _, f1, f2 in pair_relations(t_values, set(), list(known))[:limit]:
            found.append(relation("pair", (f1, f2), (f1, f2), t_values))
    # Residuals T - F1 for same-size base fields that compress T's spread the most.
    bases = []
    for field, values in cols.items():
        if field[1] != t_size or (field,) in known:
            continue
        residual = [(t - v) % modulus for t, v in zip(t_values, values)]
        spread = len(set(residual))
        if spread < info["distinct"]:
            bases.append((spread, field, residual))
    for spread, base, residual in sorted(bases)[:3]:
        seen: list[tuple] = []
        prefix = f"T - {field_name(base)}"
        for field, values in cols.items():
            if field == base or not support_ok(values) or redundant((field,), seen):
                continue
            mapping = determines(values, residual)
            if mapping is not None and len(set(mapping.values())) > 1:
                found.append(relation("residual", (base, field), (field,), residual, prefix))
                seen.append((field,))
        if not seen:
            for _, f1, f2 in pair_relations(residual, {base}, seen)[:4]:
                found.append(relation("residual", (base, f1, f2), (f1, f2), residual, prefix))
    rank = {"single": 0, "residual": 1, "pair": 2}
    found.sort(key=lambda r: (r.form == "lookup", rank[r.kind], r.keys))
    return found[:limit], info


def reduce_keys(columns_: list[list[int]], widths: list[int], values: list[int]) -> tuple[tuple, dict]:
    """Drop determinant bits that do not change the result, one member after another, so the
    masked keys still determine the values jointly. Returns (masks, masked mapping)."""
    masks = [(1 << w) - 1 for w in widths]

    def keys(trial: list[int]) -> list:
        return [tuple(v & m for v, m in zip(row, trial)) for row in zip(*columns_)]

    for i, width in enumerate(widths):
        for bit in range(width):
            trial = list(masks)
            trial[i] &= ~(1 << bit)
            if determines(keys(trial), values) is not None:
                masks = trial
    mapping = determines(keys(masks), values)
    if len(columns_) == 1:
        mapping = {k[0]: v for k, v in mapping.items()}
    return tuple(masks), mapping


FIELD_TOKEN = re.compile(r"\+(0x[0-9a-fA-F]+|\d+)\.([bwl])")


def _signed(bits: int):
    return lambda value: value - (1 << bits) if value & (1 << (bits - 1)) else value


HELPERS = {"abs": abs, "min": min, "max": max, "s8": _signed(8), "s16": _signed(16), "s32": _signed(32)}
NAMES = {"F", "rom", *HELPERS}


def compile_expr(text: str, rom: bytes):
    """Record expression -> function(record bytes) -> int. Fields are +OFF.b/.w/.l (unsigned,
    big-endian); rom.b(addr)/rom.w(addr)/rom.l(addr) read program ROM; abs/min/max and s8/s16/s32
    (signed view of a value) are available; Python syntax and precedence otherwise (& binds tighter
    than ==: parenthesise). A whole expression `FIELD == expr` compares modulo the field's width
    (word arithmetic wraps) and reports both sides on failures."""
    sizes = {"b": 1, "w": 2, "l": 4}

    class Rom:
        @staticmethod
        def b(address: int) -> int:
            return rom[address] if 0 <= address < len(rom) else -1

        @staticmethod
        def w(address: int) -> int:
            return int.from_bytes(rom[address:address + 2], "big") if 0 <= address < len(rom) - 1 else -1

        @staticmethod
        def l(address: int) -> int:
            return int.from_bytes(rom[address:address + 4], "big") if 0 <= address < len(rom) - 3 else -1

    source = FIELD_TOKEN.sub(lambda m: f"F({int(m.group(1), 0)}, {sizes[m.group(2)]})", text.strip())
    try:
        tree = ast.parse(source, mode="eval")
    except SyntaxError as error:
        raise SystemExit(f"f3a: bad record expression {text!r}: {error.msg} (fields are written +0x2a.w)")
    for node in ast.walk(tree):
        if isinstance(node, ast.Compare) and len(node.ops) > 1:
            raise SystemExit(f"f3a: {text!r} contains a chained comparison ({ast.unparse(node)}); Python reads "
                             "`a != b & c != d` as `a != (b & c) != d`. Parenthesise and join with `and`/`or`")
        if isinstance(node, ast.Name) and node.id not in NAMES:
            raise SystemExit(f"f3a: {text!r}: unknown name {node.id!r}; available: fields +OFF.b/.w/.l, "
                             "rom.b/w/l(addr), " + ", ".join(sorted(n for n in NAMES if n not in ("F", "rom"))))
        if isinstance(node, (ast.Lambda, ast.ListComp, ast.GeneratorExp, ast.DictComp, ast.SetComp, ast.NamedExpr)):
            raise SystemExit(f"f3a: {text!r}: {type(node).__name__} is not supported in record expressions")
    body = tree.body
    sides = None
    if isinstance(body, ast.Compare) and isinstance(body.ops[0], ast.Eq):
        left, right = body.left, body.comparators[0]
        sides = tuple(compile(ast.Expression(side), "<side>", "eval") for side in (left, right))
        if isinstance(left, ast.Call) and getattr(left.func, "id", "") == "F":
            modulus = 1 << (8 * left.args[1].value)
            tree = ast.Expression(ast.Compare(
                left=ast.BinOp(ast.BinOp(left, ast.Sub(), right), ast.Mod(), ast.Constant(modulus)),
                ops=[ast.Eq()], comparators=[ast.Constant(0)]))
            ast.fix_missing_locations(tree)
    code = compile(tree, "<expr>", "eval")

    def evaluate(row: bytes, expr=code) -> int:
        def F(off: int, size: int) -> int:
            return int.from_bytes(row[off:off + size], "big")
        return eval(expr, {"__builtins__": {}}, {"F": F, "rom": Rom, **HELPERS})

    evaluate.sides = sides
    return evaluate
