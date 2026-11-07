package main

import (
	"encoding/json"
	"os"
	"slices"
	"testing"
)

func TestPySetOrderMatchesCPython(t *testing.T) {
	data, err := os.ReadFile("testdata/pyset.json")
	if err != nil {
		t.Fatal(err)
	}
	var cases []struct{ In, Want []int }
	if err := json.Unmarshal(data, &cases); err != nil {
		t.Fatal(err)
	}
	for _, c := range cases {
		if got := pySetOrder(c.In); !slices.Equal(got, c.Want) {
			t.Errorf("pySetOrder(%d entries) = %v, want %v", len(c.In), got, c.Want)
		}
	}
}
