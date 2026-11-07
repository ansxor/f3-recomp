package main

import (
	"bytes"
	"os"
	"path/filepath"
)

// WriteIfChanged writes data to path unless the file already holds exactly those
// bytes, keeping mtimes stable for unchanged output. Missing parent directories
// are created. It reports whether the file was written.
func WriteIfChanged(path string, data []byte) (bool, error) {
	if existing, err := os.ReadFile(path); err == nil && bytes.Equal(existing, data) {
		return false, nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o777); err != nil {
		return false, err
	}
	if err := os.WriteFile(path, data, 0o666); err != nil {
		return false, err
	}
	return true, nil
}
