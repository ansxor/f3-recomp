package main

import (
	"errors"
	"fmt"
	"io"
)

func runPipeline(args []string, stdout, stderr io.Writer) int {
	command, err := ParseCommand(args)
	if errors.Is(err, ErrHelp) {
		fmt.Fprint(stdout, discoverHelp)
		return 0
	}
	if err != nil {
		return usageFailure(stderr, err)
	}

	game, err := LoadGame(command.Config, command.ROMDir)
	if err != nil {
		return failure(stderr, err)
	}
	analysis, err := game.Discover(DiscoverOptions{})
	if err != nil {
		return failure(stderr, err)
	}

	coverage, err := analysis.WriteCoverage(command.Output)
	if err != nil {
		return failure(stderr, err)
	}
	printDocument(stdout, coverage.Summary())
	if command.Name != Emit {
		return 0
	}
	program, err := analysis.Generate(command.Codegen)
	if err != nil {
		return failure(stderr, err)
	}
	emission, err := program.Write(command.Output)
	if err != nil {
		return failure(stderr, err)
	}
	return printDocument(stdout, emission.Summary())
}

func usageFailure(stderr io.Writer, err error) int {
	fmt.Fprint(stderr, discoverUsage)
	fmt.Fprintf(stderr, "f3-recomp: error: %s\n", err)
	return 2
}

func failure(stderr io.Writer, err error) int {
	fmt.Fprintf(stderr, "f3-recomp: %v\n", err)
	return 1
}

func printDocument(w io.Writer, fields *Fields) int {
	fmt.Fprintln(w, fields.Encode(2))
	return 0
}
