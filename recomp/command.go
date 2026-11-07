package main

import (
	"errors"
	"flag"
	"fmt"
)

type CommandName string

const (
	Discover CommandName = "discover"
	Emit     CommandName = "emit"
)

var ErrHelp = errors.New("f3-recomp: help requested")

type UsageError struct {
	Message string
}

func (e *UsageError) Error() string { return e.Message }

type Command struct {
	Name    CommandName
	Config  string
	ROMDir  string
	Output  string
	Codegen CodegenOptions
}

func ParseCommand(args []string) (Command, error) {
	command := Command{Codegen: CodegenOptions{BlocksPerFile: DefaultCodegen().BlocksPerFile}}
	fs := newFlagSet("f3-recomp")
	fs.StringVar(&command.Config, "config", "", "")
	fs.StringVar(&command.ROMDir, "rom-dir", "", "")
	fs.StringVar(&command.Output, "output", "", "ignored generated-output directory (e.g. build/landmakrj)")
	fs.IntVar(&command.Codegen.MaxBlockInstructions, "max-block-instructions", DefaultCodegen().MaxBlockInstructions, "")
	fs.StringVar(&command.Codegen.ProfileTiers, "profile-tiers", "", "retain all entries, splitting hot and cold sources using this profile")
	fs.StringVar(&command.Codegen.ProfileSlim, "profile-slim", "", "retain only executed entries from this profile (strict sparse build)")

	var positional []string
	rest := args
	for {
		if err := fs.Parse(rest); err != nil {
			if errors.Is(err, flag.ErrHelp) {
				return Command{}, ErrHelp
			}
			return Command{}, &UsageError{Message: err.Error()}
		}
		if fs.NArg() == 0 {
			break
		}
		positional = append(positional, fs.Arg(0))
		rest = fs.Args()[1:]
	}

	switch {
	case len(positional) == 0:
		return Command{}, &UsageError{Message: "the following arguments are required: command"}
	case len(positional) > 1:
		return Command{}, &UsageError{Message: "unrecognized arguments: " + fmt.Sprint(positional[1:])}
	case positional[0] != string(Discover) && positional[0] != string(Emit):
		return Command{}, &UsageError{Message: fmt.Sprintf("argument command: invalid choice: '%s' (choose from 'discover', 'emit')", positional[0])}
	}
	command.Name = CommandName(positional[0])
	for _, name := range []string{"config", "rom-dir", "output"} {
		if !visited(fs, name) {
			return Command{}, &UsageError{Message: fmt.Sprintf("the following arguments are required: --%s", name)}
		}
	}
	if visited(fs, "profile-tiers") && visited(fs, "profile-slim") {
		return Command{}, &UsageError{Message: "argument --profile-slim: not allowed with argument --profile-tiers"}
	}
	return command, nil
}
