package main

import (
	"fmt"
	"strings"
)

// Fields is a string-keyed object that remembers insertion order, like the
// Python dicts the generated JSON reports were built from.
type Fields struct {
	keys   []string
	values map[string]any
}

func NewFields(kv ...any) *Fields {
	if len(kv)%2 != 0 {
		panic("NewFields: odd number of arguments")
	}
	fields := &Fields{values: map[string]any{}}
	for i := 0; i < len(kv); i += 2 {
		key, ok := kv[i].(string)
		if !ok {
			panic(fmt.Sprintf("NewFields: key %v is not a string", kv[i]))
		}
		fields.Set(key, kv[i+1])
	}
	return fields
}

func (f *Fields) Set(key string, value any) {
	if _, ok := f.values[key]; !ok {
		f.keys = append(f.keys, key)
	}
	f.values[key] = value
}

func (f *Fields) Get(key string) (any, bool) {
	value, ok := f.values[key]
	return value, ok
}

// Keys returns the keys in insertion order; the slice must not be modified.
func (f *Fields) Keys() []string { return f.keys }

func (f *Fields) Len() int { return len(f.keys) }

func (f *Fields) Without(keys ...string) *Fields {
	copied := &Fields{values: make(map[string]any, len(f.keys))}
	for _, key := range f.keys {
		if contains(keys, key) {
			continue
		}
		copied.keys = append(copied.keys, key)
		copied.values[key] = f.values[key]
	}
	return copied
}

// Encode renders the object as JSON with json.dumps(v, indent) spacing and
// ensure_ascii escaping; indent 0 selects the compact single-line form.
func (f *Fields) Encode(indent int) string {
	var b strings.Builder
	encodeValue(&b, f, indent, 0, true)
	return b.String()
}

func contains(haystack []string, needle string) bool {
	for _, item := range haystack {
		if item == needle {
			return true
		}
	}
	return false
}
