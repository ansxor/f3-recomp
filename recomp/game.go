package main

import (
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

var (
	romKeys     = []string{"interleave", "lanes", "size"}
	romLaneKeys = []string{"crc", "file", "offset", "sha1", "size"}
)

type Game struct {
	Image    []byte
	Manifest Manifest
}

func LoadGame(configPath, romDir string) (Game, error) {
	configPath = filepath.Clean(configPath)
	romDir = filepath.Clean(romDir)
	manifest, err := LoadManifest(configPath)
	if err != nil {
		var pathErr *fs.PathError
		if errors.As(err, &pathErr) {
			return Game{}, osError(err, configPath)
		}
		return Game{}, err
	}

	gameID := any("unknown")
	if id, ok := manifest.Section("game").Value("id"); ok {
		gameID = id
	}
	rom := manifest.Section("rom")
	if !truthy(rom) {
		return Game{}, fmt.Errorf("Config '%s' is missing [rom] section.", configPath)
	}
	if err := rom.RejectUnknown("[rom]", configPath, romKeys); err != nil {
		return Game{}, err
	}

	totalSize, ok := rom.Int("size")
	if !ok || totalSize <= 0 {
		return Game{}, fmt.Errorf("Config '%s' must specify positive integer 'size' under [rom].", configPath)
	}

	interleave := 4
	if v, present := rom.Value("interleave"); present {
		interleave, ok = pyIntValue(v)
	} else {
		ok = true
	}
	lanes := rom.List("lanes")
	if !ok || interleave <= 0 || totalSize%interleave != 0 {
		return Game{}, errors.New("ROM size must be a positive multiple of its interleave.")
	}
	if len(lanes) == 0 {
		return Game{}, fmt.Errorf("Config '%s' must define at least one [[rom.lanes]].", configPath)
	}

	image := make([]byte, totalSize)
	seenOffsets := map[int]struct{}{}
	for _, rawLane := range lanes {
		lane, _ := manifestValue(rawLane)
		if err := lane.RejectUnknown("[[rom.lanes]]", configPath, romLaneKeys); err != nil {
			return Game{}, err
		}
		filename, _ := lane.String("file")
		if filename == "" {
			return Game{}, fmt.Errorf("Lane definition in '%s' is missing 'file' name.", configPath)
		}

		lanePath := filepath.Join(romDir, filename)
		if info, err := os.Stat(lanePath); err != nil || !info.Mode().IsRegular() {
			return Game{}, fmt.Errorf("Missing ROM lane file '%s' in '%s' for game '%s'.", filename, romDir, Text(gameID))
		}
		data, err := os.ReadFile(lanePath)
		if err != nil {
			return Game{}, osError(err, lanePath)
		}

		if expected, present := lane.Value("size"); present {
			if n, isInt := pyIntValue(expected); !isInt || len(data) != n {
				return Game{}, fmt.Errorf("ROM lane '%s' size mismatch: expected %s bytes, got %d bytes.",
					filename, Text(expected), len(data))
			}
		}

		if expected, _ := lane.String("crc"); expected != "" {
			actual := fmt.Sprintf("%08x", crc32.ChecksumIEEE(data))
			if !strings.EqualFold(actual, expected) {
				return Game{}, fmt.Errorf("ROM lane '%s' CRC32 mismatch: expected %s, got %s.", filename, expected, actual)
			}
		}

		if expected, _ := lane.String("sha1"); expected != "" {
			sum := sha1.Sum(data)
			actual := hex.EncodeToString(sum[:])
			if actual != strings.ToLower(expected) {
				return Game{}, fmt.Errorf("ROM lane '%s' SHA1 mismatch: expected %s, got %s.", filename, expected, actual)
			}
		}

		offset := 0
		offsetOK := true
		offsetRaw := any(0)
		if v, present := lane.Value("offset"); present {
			offsetRaw = v
			offset, offsetOK = pyIntValue(v)
		}
		if !offsetOK || offset < 0 || offset >= interleave {
			return Game{}, fmt.Errorf("ROM lane '%s' has invalid byte offset %s.", filename, Text(offsetRaw))
		}
		if len(data) != totalSize/interleave {
			return Game{}, fmt.Errorf("ROM lane '%s' does not fill the configured image.", filename)
		}
		if _, dup := seenOffsets[offset]; dup {
			return Game{}, fmt.Errorf("Duplicate lane offset %d in '%s'.", offset, configPath)
		}
		seenOffsets[offset] = struct{}{}

		for i, b := range data {
			image[offset+i*interleave] = b
		}
	}
	if len(seenOffsets) != interleave {
		return Game{}, errors.New("ROM lane configuration must cover every byte of the image.")
	}
	return Game{Image: image, Manifest: manifest}, nil
}

func (g Game) Discover(opts DiscoverOptions) (*Analysis, error) {
	return discover(g.Image, g.Manifest, opts.Decoder)
}

func truthy(v any) bool {
	switch x := v.(type) {
	case nil:
		return false
	case bool:
		return x
	case string:
		return x != ""
	case float64:
		return x != 0
	case map[string]any:
		return len(x) > 0
	}
	if n, ok := intValue(v); ok {
		return n != 0
	}
	if list, ok := asList(v); ok {
		return len(list) > 0
	}
	return true
}

func osError(err error, path string) error {
	if errors.Is(err, fs.ErrNotExist) {
		return fmt.Errorf("[Errno 2] No such file or directory: %s", Format(path))
	}
	return err
}
