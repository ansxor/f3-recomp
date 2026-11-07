package main

import (
	"cmp"
	"slices"
)

func sortedKeys[M ~map[K]V, K cmp.Ordered, V any](m M) []K {
	keys := make([]K, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	slices.Sort(keys)
	return keys
}

func sumValues(counts map[string]int) int {
	total := 0
	for _, count := range counts {
		total += count
	}
	return total
}

func intsOrEmpty(values []int) []int {
	if values == nil {
		return []int{}
	}
	return values
}
