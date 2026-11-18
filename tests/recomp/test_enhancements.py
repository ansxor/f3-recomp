import pytest

from recomp.compile.enhancements import (
    EnhancementsSpec,
    FlickerHoldSpec,
    parse_enhancements,
    render_cpp_header,
)

GROUP_PC = 0x4010
ANCHOR_PC = 0x4020
EXPECTED_HEADER = """#pragma once
#include <array>
#include <cstdint>

namespace f3rt::game_config {
struct FlickerHold {
    const char *name;
    uint8_t group_class, anchor_class, period;
    enum class Follow : uint8_t { Self, Anchor } follow;
};
inline constexpr uint32_t game_frame_counter = 0x408328;
inline constexpr FlickerHold flicker_holds[] = {
    {"object-shadow", 2, 1, 2, FlickerHold::Follow::Self},
};
}
"""


@pytest.fixture
def inputs():
    holds = [{
        "name": "object-shadow",
        "group_writer": GROUP_PC,
        "anchor_writer": ANCHOR_PC,
        "period": 2,
        "follow": "self",
    }]
    return (
        {"enhancements": {"game_frame_counter": 0x408328, "flicker_hold": holds}},
        {
            GROUP_PC: {"base": 11, "group_class": 2},
            ANCHOR_PC: {"base": 11, "group_class": 1},
        },
    )


def test_parse_spec_and_render_header(inputs):
    spec = parse_enhancements(*inputs)
    assert spec == EnhancementsSpec(0x408328, (FlickerHoldSpec("object-shadow", 2, 1, 2, "self"),))
    assert render_cpp_header(spec) == EXPECTED_HEADER


def test_runtime_only_ignores_holds(inputs):
    config, _ = inputs
    spec = parse_enhancements(config, None)
    assert spec == EnhancementsSpec()
    expected = EXPECTED_HEADER[:EXPECTED_HEADER.index("inline constexpr uint32_t")] + (
        "inline constexpr uint32_t game_frame_counter = 0x0;\n"
        "inline constexpr std::array<FlickerHold, 0> flicker_holds{};\n"
        "}\n"
    )
    assert render_cpp_header(spec) == expected


@pytest.mark.parametrize("counter", [True, 0x3FFFFE, 0x420000, 0x408329])
def test_invalid_counter(inputs, counter):
    config, writes = inputs
    config["enhancements"]["game_frame_counter"] = counter
    with pytest.raises(ValueError, match="aligned game_frame_counter"):
        parse_enhancements(config, writes)


def test_duplicate_name(inputs):
    config, writes = inputs
    holds = config["enhancements"]["flicker_hold"]
    holds.append(holds[0].copy())
    with pytest.raises(ValueError, match="Invalid or repeated flicker hold name"):
        parse_enhancements(config, writes)


@pytest.mark.parametrize(
    ("field", "value", "error"),
    [
        ("name", "object=shadow", "Invalid or repeated flicker hold name"),
        ("group_writer", 0x9998, "no sprite-group write at"),
        ("anchor_writer", 0x9998, "no sprite-group write at"),
        ("period", 1, "period must be in"),
        ("period", 256, "period must be in"),
        ("follow", "other", "follow must be self or anchor"),
    ],
)
def test_invalid_hold(inputs, field, value, error):
    config, writes = inputs
    config["enhancements"]["flicker_hold"][0][field] = value
    with pytest.raises(ValueError, match=error):
        parse_enhancements(config, writes)


def test_base_mismatch(inputs):
    config, writes = inputs
    writes[ANCHOR_PC]["base"] = 12
    with pytest.raises(ValueError, match="base registers differ"):
        parse_enhancements(config, writes)
