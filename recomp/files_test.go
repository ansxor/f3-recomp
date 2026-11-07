package main

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestWriteIfChanged(t *testing.T) {
	path := filepath.Join(t.TempDir(), "nested", "out.txt")
	written, err := WriteIfChanged(path, []byte("one"))
	if err != nil || !written {
		t.Fatalf("first write = %v, %v", written, err)
	}
	old := time.Now().Add(-time.Hour)
	if err := os.Chtimes(path, old, old); err != nil {
		t.Fatal(err)
	}
	if written, err = WriteIfChanged(path, []byte("one")); err != nil || written {
		t.Fatalf("identical write = %v, %v", written, err)
	}
	if info, _ := os.Stat(path); !info.ModTime().Equal(old.Truncate(time.Nanosecond)) && info.ModTime().Sub(old) > time.Second {
		t.Errorf("mtime changed: %v", info.ModTime())
	}
	if written, err = WriteIfChanged(path, []byte("two")); err != nil || !written {
		t.Fatalf("changed write = %v, %v", written, err)
	}
	if got, _ := os.ReadFile(path); string(got) != "two" {
		t.Errorf("content = %q", got)
	}
}

func TestWriteIfChangedDirectoryTarget(t *testing.T) {
	if _, err := WriteIfChanged(t.TempDir(), []byte("x")); err == nil {
		t.Error("writing over a directory succeeded")
	}
}
