package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"strings"
)

type CommandRunner func(args []string, stdout, stderr io.Writer) int

const discoverUsage = `usage: f3-recomp {discover,emit} --config CONFIG --rom-dir ROM_DIR --output OUTPUT
                 [--max-block-instructions N] [--profile-tiers PATH | --profile-slim PATH]
`

const discoverHelp = discoverUsage + `
Verify Taito F3 ROMs, discover code and generate C.

options:
  -h, --help            show this help message and exit
  --config CONFIG       Game ROM manifest and instruction exclusions
  --rom-dir ROM_DIR     Directory containing the selected game ROMs
  --output OUTPUT       Generated-output directory (e.g. build/landmakrj)
  --max-block-instructions N
                        Maximum instructions per emitted block
  --profile-tiers PROFILE_TIERS
                        Retain all entries, splitting hot and cold sources using
                        this profile
  --profile-slim PROFILE_SLIM
                        Retain only executed entries from this profile (strict
                        sparse build)
`

type listFlag struct{ values []string }

func (l *listFlag) String() string { return strings.Join(l.values, " ") }

func (l *listFlag) Set(value string) error {
	l.values = append(l.values, value)
	return nil
}

func newFlagSet(name string) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	return fs
}

func visited(fs *flag.FlagSet, name string) bool {
	given := false
	fs.Visit(func(f *flag.Flag) { given = given || f.Name == name })
	return given
}

func required(fs *flag.FlagSet, names ...string) error {
	var missing []string
	for _, name := range names {
		if !visited(fs, name) {
			missing = append(missing, "--"+name)
		}
	}
	if len(missing) == 0 {
		return nil
	}
	return &UsageError{Message: "the following arguments are required: " + strings.Join(missing, ", ")}
}

func isOption(arg string) bool {
	return strings.HasPrefix(arg, "-") && arg != "-"
}

// expandListFlag rewrites the historical `--configs A B C` form into repeated
// `--configs=A` options, which the stdlib flag package can parse.
func expandListFlag(args []string, names ...string) ([]string, error) {
	var expanded []string
	for i := 0; i < len(args); i++ {
		name := strings.TrimLeft(args[i], "-")
		if !isOption(args[i]) || !contains(names, name) {
			expanded = append(expanded, args[i])
			continue
		}
		values := 0
		for ; i+1 < len(args) && !isOption(args[i+1]); i++ {
			expanded = append(expanded, "--"+name+"="+args[i+1])
			values++
		}
		if values == 0 {
			return nil, &UsageError{Message: fmt.Sprintf("argument --%s: expected at least one argument", name)}
		}
	}
	return expanded, nil
}

func parseFlagSet(fs *flag.FlagSet, args []string, requiredNames, multi []string) error {
	expanded, err := expandListFlag(args, multi...)
	if err != nil {
		return err
	}
	parseErr := fs.Parse(expanded)
	if errors.Is(parseErr, flag.ErrHelp) {
		return ErrHelp
	}
	if parseErr != nil {
		message := parseErr.Error()
		if name, found := strings.CutPrefix(message, "flag needs an argument: -"); found {
			return &UsageError{Message: fmt.Sprintf("argument --%s: expected one argument", strings.TrimLeft(name, "-"))}
		}
		if err := required(fs, requiredNames...); err != nil {
			return err
		}
		if name, found := strings.CutPrefix(message, "flag provided but not defined: -"); found {
			return &UsageError{Message: "unrecognized arguments: --" + strings.TrimLeft(name, "-")}
		}
		return &UsageError{Message: message}
	}
	if err := required(fs, requiredNames...); err != nil {
		return err
	}
	if rest := fs.Args(); len(rest) > 0 {
		return &UsageError{Message: "unrecognized arguments: " + strings.Join(rest, " ")}
	}
	return nil
}

func commandFailure(err error, stdout, stderr io.Writer, prog, usage, help string) int {
	if errors.Is(err, ErrHelp) {
		fmt.Fprint(stdout, help)
		return 0
	}
	fmt.Fprint(stderr, usage)
	fmt.Fprintf(stderr, "%s: error: %s\n", prog, err)
	return 2
}
