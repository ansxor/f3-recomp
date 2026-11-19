from tools.recomp_compare import diff
from tools.recomp_compare.decisions import COLUMNS


def dump(path, row):
    path.write_text("\t".join(COLUMNS) + "\n" + ("\t".join(row) + "\n" if row else ""), encoding="utf-8")
    return str(path)


def test_decoder_diff_reports_explained_and_unexplained_cells(monkeypatch, tmp_path, capsys):
    rom = tmp_path / "rom"
    rom.write_bytes(bytes.fromhex("81 08 30 18"))

    def compare(length, flow):
        before = dump(tmp_path / "before", ["0x000000", "native", "4", "next", "-", "-", "-", "-", "-"])
        after = dump(tmp_path / "after", ["0x000000", "-", length, flow, "-", "-", "-", "-", "-"])
        monkeypatch.setattr("sys.argv", ["diff", before, after, "--columns", "length,flow", "--rom-image", str(rom)])
        return diff.main()

    assert compare("2", "next") == 0
    assert "sbcd-length\t1\t0x000000\t81 08 30 18\t4\t2" in capsys.readouterr().out
    assert compare("6", "escape") == 1
    report = capsys.readouterr().out
    assert "[length]\n0x000000\tentry\t4\t6" in report
    assert "[flow]\n0x000000\tentry\tnext\tescape" in report


def test_decoder_diff_audits_missing_ordinary_rows(monkeypatch, tmp_path, capsys):
    rom = tmp_path / "rom"
    before = dump(tmp_path / "before", None)
    after = dump(tmp_path / "after", ["0x000000", "-", "6", "next", "-", "-", "-", "-", "-"])
    monkeypatch.setattr("sys.argv", ["diff", before, after, "--columns", "length,flow", "--rom-image", str(rom)])

    # Capstone checks both CAS2 extensions, even if the first passes its mask.
    rom.write_bytes(bytes.fromhex("0e fc 00 00 05 04"))
    assert diff.main() == 0
    assert "cas2-extension\t1\t0x000000" in capsys.readouterr().out

    rom.write_bytes(bytes.fromhex("0e fc 00 00 00 04"))
    assert diff.main() == 1
    assert "[entry]\n0x000000\tentry\t<missing>\t6, next" in capsys.readouterr().out


def test_discovery_diff_explains_masked_target_but_rejects_wrong_chain(monkeypatch, tmp_path, capsys):
    rom = tmp_path / "rom"
    rom.write_bytes(bytes.fromhex("4e f9 12 00 00 06 4e 75"))
    config = tmp_path / "config.toml"
    config.write_text('[discovery]\ncoverage = "all_aligned"\n', encoding="utf-8")
    before, after = tmp_path / "before", tmp_path / "after"
    target = "0x000006\tnative\t2\tescape\t-\t-\t-\t-\t-\n"
    dump(before, ["0x000000", "native", "6", "branch:0x12000006", "-", "-", "-", "-", "-"])
    before.write_text(before.read_text() + target)
    monkeypatch.setattr("sys.argv", [
        "diff", str(before), str(after), "--rom-image", str(rom), "--config", str(config),
        "--columns", "length,flow,disposition,chain_candidates",
    ])

    def compare(chains):
        dump(after, ["0x000000", "native", "6", "branch:0x000006", "-", "-", "-", "-", chains])
        after.write_text(after.read_text() + target)
        return diff.main()

    assert compare("0x000006") == 0
    report = capsys.readouterr().out
    assert "physical-transfer\t1\t0x000000" in report
    assert "chain-source-decode\t1\t0x000000" in report

    assert compare("0x000000") == 1
    assert "[chain_candidates]\n0x000000\tentry\t-\t0x000000" in capsys.readouterr().out
