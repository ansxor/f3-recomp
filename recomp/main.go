package main

import (
	"io"
	"os"
)

var subcommands = map[string]CommandRunner{
	"game-id":              runGameID,
	"compile-roms":         runRoms,
	"compile-sprite-units": runSpriteUnits,
	"compile-sound":        runSound,
}

func main() {
	os.Exit(Invoke(os.Args[1:], os.Stdout, os.Stderr))
}

func Invoke(args []string, stdout, stderr io.Writer) int {
	if len(args) > 0 {
		if command, ok := subcommands[args[0]]; ok {
			return command(args[1:], stdout, stderr)
		}
	}
	return runPipeline(args, stdout, stderr)
}
