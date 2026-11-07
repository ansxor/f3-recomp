package main

import (
	"fmt"
	"hash/crc32"
	"io"
	"path/filepath"
	"strconv"
)

const soundUsage = `usage: compile-sound [-h] --rom-dir ROM_DIR --output OUTPUT --config CONFIG
                     [--blocks-per-file BLOCKS_PER_FILE]
                     [--profile-tiers PROFILE_TIERS | --profile-slim PROFILE_SLIM]
                     [--coverage {all_aligned}]
`

const soundHelp = soundUsage + `
Taito F3 68000 sound driver native compiler.

options:
  -h, --help            show this help message and exit
  --rom-dir ROM_DIR     Directory containing the selected game ROMs
  --output OUTPUT       Output directory for generated sources
  --config CONFIG       Game ROM manifest and instruction exclusions
  --blocks-per-file BLOCKS_PER_FILE
                        Number of instruction blocks per shard C file
  --profile-tiers PROFILE_TIERS
                        Profile for hot/cold full-coverage generation
  --profile-slim PROFILE_SLIM
                        Profile for explicit hot-only generation
  --coverage {all_aligned}
                        Compile every aligned entry in the complete loaded
                        sound region
`

type SoundCommand struct {
	ROMDir        string
	Output        string
	Config        string
	BlocksPerFile int
	ProfileTiers  string
	ProfileSlim   string
	Coverage      string
}

func ParseSoundCommand(args []string) (SoundCommand, error) {
	command := SoundCommand{Coverage: DefaultSoundOptions().CoverageMode, BlocksPerFile: DefaultSoundOptions().BlocksPerFile}
	var blocksArg string
	fs := newFlagSet("compile-sound")
	fs.StringVar(&command.ROMDir, "rom-dir", "", "")
	fs.StringVar(&command.Output, "output", "", "")
	fs.StringVar(&command.Config, "config", "", "")
	fs.StringVar(&blocksArg, "blocks-per-file", strconv.Itoa(command.BlocksPerFile), "")
	fs.StringVar(&command.ProfileTiers, "profile-tiers", "", "")
	fs.StringVar(&command.ProfileSlim, "profile-slim", "", "")
	fs.StringVar(&command.Coverage, "coverage", command.Coverage, "")
	if err := parseFlagSet(fs, args, []string{"rom-dir", "output", "config"}, nil); err != nil {
		return SoundCommand{}, err
	}
	if visited(fs, "blocks-per-file") {
		blocks, err := strconv.Atoi(blocksArg)
		if err != nil {
			return SoundCommand{}, &UsageError{Message: fmt.Sprintf("argument --blocks-per-file: invalid int value: '%s'", blocksArg)}
		}
		command.BlocksPerFile = blocks
	}
	if visited(fs, "profile-tiers") && visited(fs, "profile-slim") {
		return SoundCommand{}, &UsageError{Message: "argument --profile-slim: not allowed with argument --profile-tiers"}
	}
	if command.Coverage != "all_aligned" {
		return SoundCommand{}, &UsageError{Message: fmt.Sprintf("argument --coverage: invalid choice: '%s' (choose from 'all_aligned')", command.Coverage)}
	}
	return command, nil
}

func (c SoundCommand) Run(stdout, stderr io.Writer) int {
	fail := func(err error) int {
		fmt.Fprintf(stderr, "error: %v\n", err)
		return 1
	}
	manifest, err := LoadManifest(c.Config)
	if err != nil {
		return fail(err)
	}
	romDir, output := filepath.Clean(c.ROMDir), filepath.Clean(c.Output)

	fmt.Fprintf(stdout, "Loading sound ROM from %s...\n", romDir)
	image, err := LoadSoundImage(manifest, romDir)
	if err != nil {
		return fail(err)
	}
	gameID, _ := manifest.GameID()
	fmt.Fprintf(stdout, "Validated %s sound ROM (%d bytes, CRC: %08x)\n", gameID, len(image.Image), crc32.ChecksumIEEE(image.Image))

	fmt.Fprintf(stdout, "Compiling sound model (coverage: %s) to %s...\n", c.Coverage, output)
	program, err := image.Compile(SoundOptions{
		CoverageMode:  c.Coverage,
		BlocksPerFile: c.BlocksPerFile,
		ProfileTiers:  c.ProfileTiers,
		ProfileSlim:   c.ProfileSlim,
	})
	if err != nil {
		return fail(err)
	}
	emission, err := program.Write(output)
	if err != nil {
		return fail(err)
	}

	fmt.Fprintf(stdout, "Generated %d sources (%d/%d entries, %s, %d functions)\n",
		len(emission.Files), len(program.Entries), program.Original, program.ProfileMode, program.Stats.Functions)
	fmt.Fprintf(stdout, "Excluded %d words in %d regions\n", program.ExcludedWords, len(program.Image.Exclusions))
	fmt.Fprintf(stdout, "Supported instructions: %d\n", sumValues(program.Stats.Supported))
	fmt.Fprintf(stdout, "Actionable error stubs: %d\n", sumValues(program.Stats.Unsupported))
	fmt.Fprintln(stdout, "Compilation completed successfully.")
	return 0
}

func runSound(args []string, stdout, stderr io.Writer) int {
	command, err := ParseSoundCommand(args)
	if err != nil {
		return commandFailure(err, stdout, stderr, "compile-sound", soundUsage, soundHelp)
	}
	return command.Run(stdout, stderr)
}
