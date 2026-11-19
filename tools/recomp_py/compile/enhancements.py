import json
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class FlickerHoldSpec:
    name: str
    group_class: int
    anchor_class: int
    period: int
    follow: str


@dataclass(frozen=True)
class EnhancementsSpec:
    game_frame_counter: int = 0
    flicker_holds: tuple[FlickerHoldSpec, ...] = ()


def load_group_writes(path: Path | None) -> dict[int, dict] | None:
    if path is None:
        return None
    if not path.is_file():
        return {}
    return {int(write["pc"], 16): write for write in json.loads(path.read_text("utf-8"))["writes"]}


def parse_enhancements(config: dict, writes: dict[int, dict] | None) -> EnhancementsSpec:
    if writes is None:
        return EnhancementsSpec()
    enhancements = config.get("enhancements", {})
    holds = enhancements.get("flicker_hold", [])
    clock = 0
    rows = []
    if holds:
        clock = enhancements.get("game_frame_counter", 0)
        if type(clock) is not int or not 0x400000 <= clock < 0x420000 or clock % 2:
            raise ValueError("flicker_hold requires an aligned game_frame_counter RAM address")
        names = set()
        for hold in holds:
            name = hold["name"]
            if not isinstance(name, str) or not name or "=" in name or name in names:
                raise ValueError(f"Invalid or repeated flicker hold name {name!r}")
            names.add(name)
            for pc in (hold["group_writer"], hold["anchor_writer"]):
                if pc not in writes:
                    raise ValueError(f"{name}: no sprite-group write at {pc:#x}")
            group, anchor = (writes[hold[key]] for key in ("group_writer", "anchor_writer"))
            if group["base"] != anchor["base"]:
                raise ValueError(f"{name}: sprite-group writer base registers differ")
            period, follow = hold["period"], hold["follow"]
            if type(period) is not int or not 2 <= period <= 255:
                raise ValueError(f"{name}: period must be in 2..255")
            if follow not in ("self", "anchor"):
                raise ValueError(f"{name}: follow must be self or anchor")
            rows.append(FlickerHoldSpec(name, group["group_class"], anchor["group_class"],
                                        period, follow))
    return EnhancementsSpec(clock, tuple(rows))


def render_cpp_header(spec: EnhancementsSpec) -> str:
    rows = [
        f"    {{{json.dumps(hold.name)}, {hold.group_class}, {hold.anchor_class}, "
        f"{hold.period}, FlickerHold::Follow::{hold.follow.title()}}},\n"
        for hold in spec.flicker_holds
    ]
    table = (
        "inline constexpr FlickerHold flicker_holds[] = {\n" + "".join(rows) + "};\n"
        if rows
        else "inline constexpr std::array<FlickerHold, 0> flicker_holds{};\n"
    )
    return f"""#pragma once
#include <array>
#include <cstdint>

namespace f3rt::game_config {{
struct FlickerHold {{
    const char *name;
    uint8_t group_class, anchor_class, period;
    enum class Follow : uint8_t {{ Self, Anchor }} follow;
}};
inline constexpr uint32_t game_frame_counter = {spec.game_frame_counter:#x};
{table}}}
"""


def write_if_changed(path: Path, text: str) -> None:
    if not path.exists() or path.read_text("utf-8") != text:
        path.write_text(text, "utf-8")


def main(argv: list[str] | None = None) -> int:
    config, output, *groups = (Path(arg) for arg in (argv if argv is not None else sys.argv[1:]))
    try:
        spec = parse_enhancements(tomllib.loads(config.read_text("utf-8")),
                                  load_group_writes(groups[0] if groups else None))
    except (ValueError, KeyError) as error:
        print(f"{config}: {error}", file=sys.stderr)
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(output, render_cpp_header(spec))
    return 0


if __name__ == "__main__":
    sys.exit(main())
