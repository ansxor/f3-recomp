package main

import (
	"fmt"
	"os"
	"reflect"
	"slices"

	"github.com/pelletier/go-toml/v2"
)

// Manifest is a decoded game TOML table: integers are int64, arrays []any,
// nested tables map[string]any. The zero value is an absent table, so every
// accessor answers with a miss instead of panicking.
type Manifest struct{ table map[string]any }

func ParseManifest(data []byte) (Manifest, error) {
	table := map[string]any{}
	if err := toml.Unmarshal(data, &table); err != nil {
		return Manifest{}, err
	}
	return Manifest{table}, nil
}

func LoadManifest(path string) (Manifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Manifest{}, err
	}
	manifest, err := ParseManifest(data)
	if err != nil {
		return Manifest{}, fmt.Errorf("%s: %w", path, err)
	}
	return manifest, nil
}

func ManifestOf(table map[string]any) Manifest { return Manifest{table} }

func manifestValue(v any) (Manifest, bool) {
	table, ok := v.(map[string]any)
	return Manifest{table}, ok
}

func (m Manifest) Table() map[string]any { return m.table }

func (m Manifest) Section(key string) Manifest {
	section, _ := m.table[key].(map[string]any)
	return Manifest{section}
}

func (m Manifest) Value(key string) (any, bool) {
	v, ok := m.table[key]
	return v, ok
}

func (m Manifest) String(key string) (string, bool) {
	s, ok := m.table[key].(string)
	return s, ok
}

// Int returns the integer at key; a bool counts as 0 or 1.
func (m Manifest) Int(key string) (int, bool) { return intValue(m.table[key]) }

func (m Manifest) Bool(key string) (bool, bool) {
	b, ok := m.table[key].(bool)
	return b, ok
}

func (m Manifest) List(key string) []any { return listValue(m.table[key]) }

func (m Manifest) SortedKeys() []string { return sortedKeys(m.table) }

func (m Manifest) GameID() (string, bool) { return m.Section("game").String("id") }

func (m Manifest) RejectUnknown(what, configPath string, allowed []string) error {
	var unknown []string
	for key := range m.table {
		if !slices.Contains(allowed, key) {
			unknown = append(unknown, key)
		}
	}
	if len(unknown) == 0 {
		return nil
	}
	slices.Sort(unknown)
	return fmt.Errorf("Config '%s': unknown key(s) %s in %s", configPath, Format(unknown), what)
}

func intValue(v any) (int, bool) {
	rv := reflect.ValueOf(v)
	switch rv.Kind() {
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return int(rv.Int()), true
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		return int(rv.Uint()), true
	}
	return 0, false
}

func listValue(v any) []any {
	list, _ := asList(v)
	return list
}

func asList(v any) ([]any, bool) {
	if list, ok := v.([]any); ok {
		return list, true
	}
	rv := reflect.ValueOf(v)
	if !rv.IsValid() || rv.Kind() != reflect.Slice {
		return nil, false
	}
	list := make([]any, rv.Len())
	for i := range list {
		list[i] = rv.Index(i).Interface()
	}
	return list, true
}
