package main

import (
	"fmt"
	"io"
)

const unitsUsage = "usage: sprite_units.py [-h] --config CONFIG --output-dir OUTPUT_DIR\n"

const unitsHelp = unitsUsage + `
Write sprite_units.h/.hpp for one game config (no ROM needed).

options:
  -h, --help            show this help message and exit
  --config CONFIG
  --output-dir OUTPUT_DIR
`

type SpriteUnitsCommand struct {
	Config    string
	OutputDir string
}

func ParseSpriteUnitsCommand(args []string) (SpriteUnitsCommand, error) {
	command := SpriteUnitsCommand{}
	fs := newFlagSet("sprite_units.py")
	fs.StringVar(&command.Config, "config", "", "")
	fs.StringVar(&command.OutputDir, "output-dir", "", "")
	if err := parseFlagSet(fs, args, []string{"config", "output-dir"}, nil); err != nil {
		return SpriteUnitsCommand{}, err
	}
	return command, nil
}

func (c SpriteUnitsCommand) Run(stdout, stderr io.Writer) int {
	manifest, err := LoadManifest(c.Config)
	if err != nil {
		fmt.Fprintf(stderr, "sprite_units.py: %v\n", err)
		return 1
	}
	units, err := ParseUnits(manifest)
	if err != nil {
		fmt.Fprintf(stderr, "%s: %v\n", c.Config, err)
		return 1
	}
	if err := units.Write(c.OutputDir); err != nil {
		fmt.Fprintf(stderr, "sprite_units.py: %v\n", err)
		return 1
	}
	return 0
}

func runSpriteUnits(args []string, stdout, stderr io.Writer) int {
	command, err := ParseSpriteUnitsCommand(args)
	if err != nil {
		return commandFailure(err, stdout, stderr, "sprite_units.py", unitsUsage, unitsHelp)
	}
	return command.Run(stdout, stderr)
}
