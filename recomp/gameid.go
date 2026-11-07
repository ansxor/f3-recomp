package main

import (
	"fmt"
	"io"
)

const gameIDUsage = "usage: f3-recomp game-id [-h] --config CONFIG --expect EXPECT\n"

const gameIDHelp = gameIDUsage + `
Check that a game config's [game].id matches its directory name.

options:
  -h, --help       show this help message and exit
  --config CONFIG
  --expect EXPECT
`

type GameIDCommand struct {
	Config string
	Expect string
}

func ParseGameIDCommand(args []string) (GameIDCommand, error) {
	command := GameIDCommand{}
	fs := newFlagSet("f3-recomp game-id")
	fs.StringVar(&command.Config, "config", "", "")
	fs.StringVar(&command.Expect, "expect", "", "")
	if err := parseFlagSet(fs, args, []string{"config", "expect"}, nil); err != nil {
		return GameIDCommand{}, err
	}
	return command, nil
}

func (c GameIDCommand) Run(stdout, stderr io.Writer) int {
	manifest, err := LoadManifest(c.Config)
	if err != nil {
		failure(stderr, err)
		return 1
	}
	id, ok := manifest.GameID()
	if !ok {
		fmt.Fprintf(stderr, "f3-recomp: %s: missing [game].id\n", c.Config)
		return 1
	}
	if id != c.Expect {
		fmt.Fprintf(stderr, "Configured game id does not match directory: %s\n", c.Config)
		return 1
	}
	return 0
}

func runGameID(args []string, stdout, stderr io.Writer) int {
	command, err := ParseGameIDCommand(args)
	if err != nil {
		return commandFailure(err, stdout, stderr, "f3-recomp game-id", gameIDUsage, gameIDHelp)
	}
	return command.Run(stdout, stderr)
}
