package main

import (
	"bytes"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"math/big"
	"os"
	"path/filepath"
	"strings"
)

var RegionNames = []string{"rom", "sprites", "sprites_hi", "tiles", "tiles_hi", "sound", "samples", "eeprom"}

var errMirrorSize = errors.New("invalid mirrored region size")

type Lane struct {
	File                                        string
	Size                                        int
	CRC, SHA1                                   string
	Offset, Stride, Group, SourceOffset, Length int
	ShortSize                                   int
	ShortCRC, ShortSHA1                         string
	WordSwap                                    bool
}

type RegionSpec struct {
	Size   int
	Mapped int
	Fill   int
	Lanes  []Lane
}

type identity struct {
	size                int
	crc, sha1           string
	shortSize           int
	shortCRC, shortSHA1 string
}

func (l Lane) identity() identity {
	return identity{l.Size, l.CRC, l.SHA1, l.ShortSize, l.ShortCRC, l.ShortSHA1}
}

func (m Manifest) RegionSpec(name string) (RegionSpec, error) {
	raw, present := m.Value(name)
	if !present {
		if name == "eeprom" {
			raw = map[string]any{"size": 0, "lanes": []any{}}
		} else {
			return RegionSpec{}, fmt.Errorf("missing %s region", name)
		}
	}
	region, ok := manifestValue(raw)
	if !ok {
		return RegionSpec{}, fmt.Errorf("Invalid %s region size/fill", name)
	}
	rawSize, hasSize := region.Value("size")
	if !hasSize {
		return RegionSpec{}, fmt.Errorf("Invalid %s region size/fill", name)
	}
	size, sizeOK := pyIntValue(rawSize)
	fill := 0
	if rawFill, present := region.Value("fill"); present {
		var fillOK bool
		if fill, fillOK = pyIntValue(rawFill); !fillOK {
			fill = -1
		}
	}
	if !sizeOK || size < 0 || (fill != 0 && fill != 255) {
		return RegionSpec{}, fmt.Errorf("Invalid %s region size/fill", name)
	}
	if name == "eeprom" {
		mirror, hasMirror := region.Value("mirror_size")
		if mirrorSize, ok := pyIntValue(mirror); (size != 0 && size != 128) || (hasMirror && (!ok || mirrorSize != size)) {
			return RegionSpec{}, fmt.Errorf("EEPROM region must be absent or exactly 128 bytes")
		}
	}
	rawLanes := []any(nil)
	if raw, present := region.Value("lanes"); present {
		var isList bool
		if rawLanes, isList = asList(raw); !isList {
			return RegionSpec{}, fmt.Errorf("Invalid %s lanes", name)
		}
	}
	interleave := any(1)
	if raw, present := region.Value("interleave"); present {
		interleave = raw
	}
	identities := map[string]identity{}
	var lanes []Lane
	for _, rawLane := range rawLanes {
		lane, err := parseLane(rawLane, name, size, interleave)
		if err != nil {
			return RegionSpec{}, err
		}
		if previous, seen := identities[lane.File]; seen && previous != lane.identity() {
			return RegionSpec{}, fmt.Errorf("Inconsistent physical chip identity: %s", lane.File)
		}
		identities[lane.File] = lane.identity()
		lanes = append(lanes, lane)
	}
	if size != 0 && len(lanes) == 0 {
		return RegionSpec{}, fmt.Errorf("Missing %s ROM lanes", name)
	}
	mapped, ok := mappedSize(region, size)
	if !ok {
		return RegionSpec{}, fmt.Errorf("%w: %s", errMirrorSize, name)
	}
	return RegionSpec{Size: size, Mapped: mapped, Fill: fill, Lanes: lanes}, nil
}

func mappedSize(region Manifest, size int) (mapped int, ok bool) {
	mapped = size
	if raw, present := region.Value("mirror_size"); present {
		if mapped, ok = pyIntValue(raw); !ok {
			return 0, false
		}
	}
	if mapped < size || (size != 0 && mapped%size != 0) || (size == 0 && mapped != 0) {
		return mapped, false
	}
	return mapped, true
}

func laneInts(source map[string]any, keys ...string) ([]int, bool) {
	values := make([]int, len(keys))
	for i, key := range keys {
		var ok bool
		if values[i], ok = pyIntValue(source[key]); !ok {
			return nil, false
		}
	}
	return values, true
}

func checkHex(value any, digits int) (string, bool) {
	s, ok := value.(string)
	if !ok || len(s) != digits {
		return "", false
	}
	return s, true
}

func parseLane(rawLane any, name string, regionSize int, interleave any) (Lane, error) {
	source, ok := manifestValue(rawLane)
	if !ok {
		return Lane{}, fmt.Errorf("Invalid %s lane: %s", name, Format(rawLane))
	}
	file, ok := source.String("file")
	if !ok {
		return Lane{}, fmt.Errorf("Invalid %s lane: missing file", name)
	}
	lane := Lane{File: file, WordSwap: false}
	if raw, present := source.Value("word_swap"); present {
		if lane.WordSwap, ok = raw.(bool); !ok {
			return Lane{}, fmt.Errorf("Invalid %s word_swap: %s", name, file)
		}
	}
	rawSize, _ := source.Value("size")
	rawCRC, _ := source.Value("crc")
	rawSHA1, _ := source.Value("sha1")
	withDefault := func(key string, def any) any {
		if raw, present := source.Value(key); present {
			return raw
		}
		return def
	}
	geometry := map[string]any{
		"size":          rawSize,
		"source_offset": withDefault("source_offset", 0),
		"offset":        withDefault("offset", 0),
		"stride":        withDefault("stride", interleave),
		"group":         withDefault("group", 1),
	}
	if ints, ok := laneInts(geometry, "size", "source_offset"); ok {
		geometry["length"] = withDefault("length", ints[0]-ints[1])
	}
	values, ok := laneInts(geometry, "size", "source_offset", "length", "offset", "stride", "group")
	if !ok {
		return Lane{}, fmt.Errorf("Invalid %s lane geometry", name)
	}
	lane.Size, lane.SourceOffset, lane.Length, lane.Offset, lane.Stride, lane.Group =
		values[0], values[1], values[2], values[3], values[4], values[5]
	if lane.Size <= 0 || lane.SourceOffset < 0 || lane.Length <= 0 || lane.SourceOffset+lane.Length > lane.Size ||
		lane.Group <= 0 || lane.Stride < lane.Group || lane.Offset < 0 || lane.Length%lane.Group != 0 {
		return Lane{}, fmt.Errorf("Invalid %s lane geometry: %s", name, file)
	}
	if lane.Offset+(lane.Length/lane.Group-1)*lane.Stride+lane.Group > regionSize {
		return Lane{}, fmt.Errorf("%s lane exceeds region: %s", name, file)
	}
	if lane.CRC, ok = checkHex(rawCRC, 8); !ok {
		return Lane{}, fmt.Errorf("Invalid %s crc: %s", name, file)
	}
	if lane.SHA1, ok = checkHex(rawSHA1, 40); !ok {
		return Lane{}, fmt.Errorf("Invalid %s sha1: %s", name, file)
	}
	if err := checkDigests(lane.CRC, lane.SHA1); err != nil {
		return Lane{}, err
	}
	if raw, present := source.Value("short_size"); present {
		if lane.ShortSize, ok = pyIntValue(raw); !ok || lane.ShortSize <= 0 || lane.ShortSize >= lane.Size {
			return Lane{}, fmt.Errorf("Invalid short dump size: %s", file)
		}
		shortCRC, _ := source.Value("short_crc")
		if lane.ShortCRC, ok = checkHex(shortCRC, 8); !ok {
			return Lane{}, fmt.Errorf("Invalid short_crc: %s", file)
		}
		shortSHA1, _ := source.Value("short_sha1")
		if lane.ShortSHA1, ok = checkHex(shortSHA1, 40); !ok {
			return Lane{}, fmt.Errorf("Invalid short_sha1: %s", file)
		}
		if err := checkDigests(lane.ShortCRC, lane.ShortSHA1); err != nil {
			return Lane{}, err
		}
	}
	return lane, nil
}

func checkDigests(digests ...string) error {
	for _, digest := range digests {
		if _, ok := new(big.Int).SetString(digest, 16); !ok {
			return fmt.Errorf("invalid literal for int() with base 16: %s", Format(digest))
		}
	}
	return nil
}

func validate(data []byte, lane Lane, short bool) error {
	size, crc, digest := lane.Size, lane.CRC, lane.SHA1
	if short {
		size, crc, digest = lane.ShortSize, lane.ShortCRC, lane.ShortSHA1
	}
	if len(data) != size {
		return fmt.Errorf("ROM chip %s size mismatch", Format(lane.File))
	}
	if want, _ := ParseNumber(crc, 16); int(crc32.ChecksumIEEE(data)) != want {
		return fmt.Errorf("ROM chip %s CRC32 mismatch", Format(lane.File))
	}
	if sum := sha1.Sum(data); hex.EncodeToString(sum[:]) != strings.ToLower(digest) {
		return fmt.Errorf("ROM chip %s SHA1 mismatch", Format(lane.File))
	}
	return nil
}

func LoadRegion(m Manifest, name string, romDir string) ([]byte, error) {
	spec, err := m.RegionSpec(name)
	if err != nil {
		if errors.Is(err, errMirrorSize) {
			return nil, fmt.Errorf("Invalid %s mirrored size", name)
		}
		return nil, err
	}
	image := bytes.Repeat([]byte{byte(spec.Fill)}, spec.Size)
	chips := map[string][]byte{}
	remaining := map[string]int{}
	for _, lane := range spec.Lanes {
		remaining[lane.File]++
	}
	for _, lane := range spec.Lanes {
		data, loaded := chips[lane.File]
		if !loaded {
			if data, err = os.ReadFile(filepath.Join(romDir, lane.File)); err != nil {
				return nil, err
			}
			if len(data) == lane.ShortSize && lane.ShortSize != 0 {
				if err := validate(data, lane, true); err != nil {
					return nil, err
				}
				data = append(data, bytes.Repeat([]byte{0xFF}, lane.Size-len(data))...)
			}
			if err := validate(data, lane, false); err != nil {
				return nil, err
			}
			chips[lane.File] = data
		}
		start, length := lane.SourceOffset, lane.Length
		if lane.WordSwap {
			if lane.Group != 1 || lane.Stride != 1 || length%2 != 0 {
				return nil, fmt.Errorf("word_swap requires group=1, stride=1 and even length")
			}
			for i := 0; i < length; i += 2 {
				image[lane.Offset+i] = data[start+i+1]
				image[lane.Offset+i+1] = data[start+i]
			}
		} else {
			for b := range lane.Group {
				for i := range length / lane.Group {
					image[lane.Offset+b+i*lane.Stride] = data[start+b+i*lane.Group]
				}
			}
		}
		if remaining[lane.File]--; remaining[lane.File] == 0 {
			delete(chips, lane.File)
		}
	}
	if spec.Size == 0 {
		return []byte{}, nil
	}
	return bytes.Repeat(image, spec.Mapped/spec.Size), nil
}
