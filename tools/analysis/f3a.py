"""f3a as a library for a Python kernel: the same analysis as `tools/f3a`, returned as rows with raw
fields instead of text, plus direct access to frame dumps and ROM.

    import sys; sys.path.insert(0, "tools")          # from the repository root
    from analysis import f3a
    g = f3a.game("commandw")                          # analysed once per kernel, then cached
    g.flow("irq2", tree=2)                            # every CLI command: g.<command>(positional, option=value)
    help(g.writes)                                    # that command's options (the CLI's --help)
    rows = g.writes(region="sprites")                 # Result: list of Row (row.pc, row.region, row.where, ...)
    rows.where(region="sprites").column("pc")
    g.dis("sub_009b32", from_=0x9d32, to=0x9e00)      # a window of a long routine (`from` is spelled from_)
    g.words(0x2ff42, 17); g.longs(0xa30e, 4)          # ROM tables as ints
    run = g.run(frames=1300, dump_start=1190, dump_every=1, keep="mainram.bin,graphics.bin", out="build/f3a/x")
    run.records(base="a5+0", stride=0x100, count=5, frame=1200, check="(+0x1.b & 0x80) != 0")
    run.w(1200, 0x408328)                             # word at an address in the frame-1200 dump
    [r.w(0x2) for r in run.table(0x410000, 0x100, 5, frame=1200)]
    run.series("a5-0x7cd8", 2)                        # (frame, value) for every dump
    g.run(frames=8000, inputs="play.txt", until=["sub_09d72a"]).log.where(reached=False)   # scripted input

Options are the CLI's long options with `_` for `-` (dump_every=1); True switches a flag on; ints go to
address options as hex; frame=N on records/sprites means that one frame (frames="A:B" a span). Rows
print as the CLI line; `row.fields` lists what a row carries. A Result shows its header notes and
caveats with the rows, capped at 60 lines when displayed or printed; `.text` is everything,
`.show(n)` prints n lines. Errors the CLI would print raise F3AError. `g.core` is the underlying
analysis (analysis.game.Game) for anything the commands do not expose.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import keyword
import re
from pathlib import Path

from . import cli
from .game import Game as Core, canonical

__all__ = ["game", "open_run", "help_text", "F3AError", "Row", "Result", "Game", "Run"]


class F3AError(Exception):
    """What the CLI would have printed as `f3a: ...` and exited on."""


class Row:
    """One output row: prints as the CLI line; fields as attributes (row.pc) or items (row["pc"])."""
    __slots__ = ("line", "fields")

    def __init__(self, line: str, fields: dict):
        self.line, self.fields = line, fields

    def __getattr__(self, name: str):
        try:
            return self.fields[name]
        except KeyError:
            raise AttributeError(f"row has no field {name!r}; fields: {', '.join(self.fields)}") from None

    def __getitem__(self, name: str):
        return self.fields[name]

    def get(self, name: str, default=None):
        return self.fields.get(name, default)

    def __repr__(self) -> str:
        return self.line


class Result(list):
    """Rows of one command, plus the notes (headers, caveats, footers) in output order."""
    SHOWN = 60

    def __init__(self, rows=(), items=None):
        super().__init__(rows)
        self.items = items if items is not None else list(rows)  # notes (str) and rows, as printed

    @property
    def notes(self) -> list[str]:
        return [item for item in self.items if isinstance(item, str)]

    @property
    def text(self) -> str:
        return "\n".join(item if isinstance(item, str) else item.line for item in self.items)

    def where(self, predicate=None, **equal) -> "Result":
        """Rows matching a predicate and/or field values (row.region == "sprites"); notes are kept."""
        def keep(row: Row) -> bool:
            return (predicate is None or predicate(row)) and all(row.get(k) == v for k, v in equal.items())
        kept = [row for row in self if keep(row)]
        ids = {id(row) for row in kept}
        return Result(kept, [i for i in self.items if isinstance(i, str) or id(i) in ids])

    def column(self, name: str) -> list:
        return [row.get(name) for row in self]

    def __str__(self) -> str:
        return self.__repr__()

    def __repr__(self) -> str:
        lines = self.text.splitlines()
        if len(lines) <= self.SHOWN:
            return self.text
        return "\n".join(lines[:self.SHOWN]) + f"\n… {len(lines) - self.SHOWN} more lines ({len(self)} rows); " \
            "result.text for all, result.show(n), result.where(...) to filter"

    def show(self, lines: int | None = None) -> None:
        """Print the whole output, or its first `lines` lines."""
        text = self.text.splitlines()
        print("\n".join(text if lines is None else text[:lines]))


class _Lines:
    """stdout replacement that turns printed lines into notes."""

    def __init__(self, items: list):
        self.items, self.buffer = items, ""

    def write(self, text: str) -> int:
        self.buffer += text
        while "\n" in self.buffer:
            line, self.buffer = self.buffer.split("\n", 1)
            self.items.append(line)
        return len(text)

    def flush(self) -> None:
        if self.buffer:
            self.items.append(self.buffer)
            self.buffer = ""


_PARSER: argparse.ArgumentParser | None = None


def _subparser(command: str) -> argparse.ArgumentParser:
    global _PARSER
    _PARSER = _PARSER or cli.build_parser()
    commands = next(a for a in _PARSER._actions if isinstance(a, argparse._SubParsersAction)).choices
    if command not in commands:
        raise F3AError(f"no command {command!r}; commands: {', '.join(commands)}")
    return commands[command]


def _text(value, action: argparse.Action | None) -> str:
    if isinstance(value, int) and not isinstance(value, bool):
        return str(value) if action is not None and action.type is int else hex(value)
    return str(value)


def help_text(command: str) -> str:
    """The CLI help of one command: its options, their meaning and syntax."""
    return _subparser(command).format_help()


def _argv(command: str, positional, options: dict) -> list[str]:
    parser = _subparser(command)
    by_option = {s: a for a in parser._actions for s in a.option_strings}
    argv = [command] + [_text(p, None) for p in positional]
    for key, value in options.items():
        option = "--" + key.rstrip("_").replace("_", "-")  # from_ → --from (Python keywords)
        action = by_option.get(option)
        if action is None:
            names = sorted(s[2:].replace("-", "_") + ("_" if keyword.iskeyword(s[2:]) else "")
                           for s in by_option if s.startswith("--") and s != "--help")
            raise F3AError(f"{command} has no option {key!r}; options: {', '.join(names)}")
        if value is None or value is False:
            continue
        if value is True:
            argv.append(option)
        elif key == "frames" and isinstance(value, tuple):
            argv += [option, f"{value[0]}:{value[1]}"]  # frames=(600, 603) → 600:603
        elif key == "frames" and isinstance(value, int) and action.type is not int:
            argv += [option, f"{value}:{value}"]  # frames=600 → just frame 600
        elif isinstance(value, (list, tuple)):
            if isinstance(action, argparse._AppendAction):
                for item in value:
                    argv += [option, _text(item, action)]
            else:
                argv += [option] + [_text(item, action) for item in value]
        else:
            argv += [option, _text(value, action)]
    return argv


def _call(command: str, positional=(), options: dict | None = None, core: Core | None = None) -> Result:
    options = dict(options or {})
    argv = _argv(command, positional, options)
    errors = io.StringIO()
    try:
        with contextlib.redirect_stderr(errors):
            args = _subparser(command).parse_args(argv[1:])
    except SystemExit:
        reason = errors.getvalue().strip().splitlines()[-1:] or ["?"]
        raise F3AError(f"{command} {' '.join(argv[1:])}: {reason[0].split('error: ', 1)[-1]}") from None
    if core is not None:
        args._game, args.game = core, core.id
    if hasattr(args, "limit") and "limit" not in options:
        args.limit = 0  # a library caller filters rows itself
    items: list = []
    lines = _Lines(items)

    def sink(line: str, fields: dict) -> None:
        lines.flush()
        items.append(Row(line, fields))

    args._sink = sink
    with contextlib.redirect_stdout(lines):
        try:
            args.func(args)
        except SystemExit as stop:
            if stop.code not in (None, 0):
                lines.flush()
                raise F3AError(str(stop.code).removeprefix("f3a: ")) from None
    lines.flush()
    return Result([i for i in items if isinstance(i, Row)], items)


# ---------------------------------------------------------------- static analysis
_GAMES: dict[tuple, "Game"] = {}


def game(game_id: str, rom_dir: str | None = None, profile: str | None = None, symbols: str | None = None,
         a5: int | None = None) -> "Game":
    """The analysed program of games/<game_id> (cached per kernel; arguments as the CLI's)."""
    key = (game_id, rom_dir, profile, symbols, a5)
    if key not in _GAMES:
        _GAMES[key] = Game(game_id, rom_dir, profile, symbols, a5)
    return _GAMES[key]


class Game:
    def __init__(self, game_id: str, rom_dir=None, profile=None, symbols=None, a5=None):
        namespace = argparse.Namespace(game=game_id, global_game=None, rom_dir=rom_dir, profile=profile,
                                       symbols=symbols, a5=hex(a5) if isinstance(a5, int) else a5)
        try:
            self.core: Core = cli.load(namespace)
        except SystemExit as stop:
            raise F3AError(str(stop.code).removeprefix("f3a: ")) from None
        self.id = game_id

    def __repr__(self) -> str:
        return f"f3a.game({self.id!r}): {len(self.core.routines)} routines, a5={hex(self.core.a5 or 0)}, " \
               f"profile={'yes' if self.core.hits else 'no'}"

    def call(self, command: str, *positional, **options) -> Result:
        """Any CLI command by name: g.call("xref", "a5-0x7cd7", sort="hits")."""
        return _call(command, positional, options, self.core)

    def vectors(self, **options) -> Result:
        return self.call("vectors", **options)

    def dis(self, target, **options) -> Result:
        return self.call("dis", target, **options)

    def xref(self, *targets, **options) -> Result:
        return self.call("xref", *targets, **options)

    def flow(self, target, **options) -> Result:
        return self.call("flow", target, **options)

    def writes(self, *targets, **options) -> Result:
        return self.call("writes", *targets, **options)

    def struct(self, routine, reg, **options) -> Result:
        return self.call("struct", routine, reg, **options)

    def graph(self, **options) -> Result:
        return self.call("graph", **options)

    def mametap(self, **options) -> Result:
        return self.call("mametap", **options)

    def discover(self, log=None, **options) -> Result:
        return self.call("discover", *([str(log)] if log else []), **options)

    def run(self, out: str | Path | None = None, **options) -> "Run":
        """Headless run (options as `f3a run`); returns the Run, whose .log is the run's output."""
        frames = options.get("frames", 1200)
        out = Path(out) if out else cli.default_out(self.core, f"lib{frames}")
        log = self.call("run", out=str(out), **options)
        return Run(out, self, log)
    def profile(self, **options) -> Result:
        return self.call("profile", **options)


    def address(self, text) -> int:
        """Resolve a5-0x7cd7, irq2, sub_009b32, symbols or hex text to an address."""
        return text if isinstance(text, int) else cli.resolve_target(self.core, str(text))

    def label(self, address: int) -> str:
        return self.core.owner_label(address)

    def rom(self, address, size: int) -> bytes:
        """Program ROM bytes."""
        address = self.address(address)
        return bytes(self.core.rom[address:address + size])

    def words(self, address, count: int, signed: bool = False) -> list[int]:
        """count big-endian words from ROM (tables; signed=True for offset tables)."""
        data = self.rom(address, 2 * count)
        return [int.from_bytes(data[i:i + 2], "big", signed=signed) for i in range(0, len(data), 2)]

    def longs(self, address, count: int) -> list[int]:
        """count big-endian longs from ROM (pointer tables)."""
        data = self.rom(address, 4 * count)
        return [int.from_bytes(data[i:i + 4], "big") for i in range(0, len(data), 4)]


for _name in ("vectors", "dis", "xref", "flow", "writes", "struct", "graph", "mametap", "discover", "run", "profile"):
    _method = getattr(Game, _name)
    _method.__doc__ = (f"`f3a {_name}` as a call: positional arguments as the CLI's, long options as keywords "
                       f"(dump_every=1, from_=...). The command's help:\n\n" + help_text(_name))


# ---------------------------------------------------------------- dumps
def open_run(path: str | Path, game_id: str | None = None) -> "Run":
    """An existing `f3a run --out` directory (its game is recorded there by `f3a run`)."""
    path = Path(path)
    if game_id is None:
        for marker in (path / "game", path.parent / "game"):
            if marker.is_file():
                game_id = marker.read_text().strip()
                break
        else:
            raise F3AError(f"{path} does not record its game; pass game_id")
    return Run(path, game(game_id), None)


class Record:
    """One record of a frame dump: r.b/w/l(offset) read big-endian unsigned values."""
    __slots__ = ("frame", "index", "address", "bytes")

    def __init__(self, frame: int, index: int, address: int, data: bytes):
        self.frame, self.index, self.address, self.bytes = frame, index, address, data

    def b(self, offset: int) -> int:
        return self.bytes[offset]

    def w(self, offset: int) -> int:
        return int.from_bytes(self.bytes[offset:offset + 2], "big")

    def l(self, offset: int) -> int:
        return int.from_bytes(self.bytes[offset:offset + 4], "big")

    def __repr__(self) -> str:
        return f"frame {self.frame} [{self.index}] {self.address:#08x}  {self.bytes[:32].hex(' ', 2)}"


class Run:
    """A run directory: CLI dump commands bound to it, plus raw memory of each dumped frame."""

    def __init__(self, path: Path, game: Game, log: Result | None):
        self.path, self.game, self.log = Path(path), game, log
        self._cache: dict[tuple[int, str], bytes] = {}

    def __repr__(self) -> str:
        frames = self.frames
        span = f"{frames[0]}..{frames[-1]} ({len(frames)} frames)" if frames else "no dumps"
        return f"f3a run {self.path} [{self.game.id}]: dumps {span}"

    @property
    def frames(self) -> list[int]:
        try:
            return [n for n, _ in cli.load_frames(self.path, None)]
        except SystemExit:
            return []

    def records(self, frame: int | None = None, **options) -> Result:
        """`f3a records` on this run; frame=N is the one dumped frame N."""
        if frame is not None:
            options["frames"] = f"{frame}:{frame}"
        return self.game.call("records", str(self.path), **options)

    def sprites(self, frame: int | None = None, **options) -> Result:
        """`f3a sprites` on this run; frame=N decodes that one frame."""
        if frame is not None:
            options["frames"] = f"{frame}:{frame}"
        return self.game.call("sprites", str(self.path), **options)

    @property
    def output(self) -> str:
        """The game's own output of the run (run.txt): VIDEO/DISCOVERY/flicker summaries."""
        path = self.path / "run.txt"
        return path.read_text() if path.is_file() else ""

    @property
    def summary(self) -> dict[str, str]:
        """key=value pairs of the game's summary lines (frame_crc, cycles, video_write, ...)."""
        return {k: v for line in self.output.splitlines() for k, v in re.findall(r"(\w+)=(\S+)", line)}

    def discover(self, **options) -> Result:
        return self.game.discover(self.path / "discovery.log", **options)

    def _file(self, frame: int, name: str) -> bytes:
        key = (frame, name)
        if key not in self._cache:
            try:
                (n, path), = cli.load_frames(self.path, f"{frame}:{frame}")
                self._cache[key] = (path / name).read_bytes()
            except (SystemExit, ValueError):
                raise F3AError(f"frame {frame} is not dumped under {self.path}") from None
            except FileNotFoundError:
                raise F3AError(f"frame {frame} has no {name} (run with --keep including it)") from None
        return self._cache[key]

    def mem(self, frame: int, address, size: int = 1) -> bytes:
        """Bytes at an address (int, hex text or a5-0x7cd8) in one frame's dump."""
        address = canonical(self.game.address(address) if not isinstance(address, int) else address)
        try:
            name, offset = cli.dump_location(address)
        except SystemExit as stop:
            raise F3AError(str(stop.code).removeprefix("f3a: ")) from None
        return self._file(frame, name)[offset:offset + size]

    def b(self, frame: int, address) -> int:
        return self.mem(frame, address, 1)[0]

    def w(self, frame: int, address) -> int:
        return int.from_bytes(self.mem(frame, address, 2), "big")

    def l(self, frame: int, address) -> int:
        return int.from_bytes(self.mem(frame, address, 4), "big")

    def series(self, address, size: int = 1, frames=None) -> list[tuple[int, int]]:
        """(frame, value) for every dumped frame (or the given frames)."""
        return [(n, int.from_bytes(self.mem(n, address, size), "big")) for n in (frames or self.frames)]

    def table(self, base, stride: int, count: int, frame: int, size: int | None = None) -> list[Record]:
        """count records of `size` (default stride) bytes from base in one frame."""
        base = self.game.address(base) if not isinstance(base, int) else base
        return [Record(frame, k, base + k * stride, self.mem(frame, base + k * stride, size or stride))
                for k in range(count)]


for _name in ("records", "sprites"):
    _method = getattr(Run, _name)
    _method.__doc__ += "\n\nThe command's help (positional dumps directory is this run):\n\n" + help_text(_name)
