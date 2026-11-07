package main

const (
	pySetMinSize     = 8
	pySetLinearProbe = 9
	pySetPerturbBits = 5
)

func pySetOrder(entries []int) []int {
	table := make([]int, pySetMinSize)
	used := make([]bool, pySetMinSize)
	fill := 0

	insertClean := func(table []int, used []bool, key int) {
		mask := len(table) - 1
		i := key & mask
		perturb := uint(key)
		for {
			if !used[i] {
				table[i], used[i] = key, true
				return
			}
			if i+pySetLinearProbe <= mask {
				for j := 1; j <= pySetLinearProbe; j++ {
					if !used[i+j] {
						table[i+j], used[i+j] = key, true
						return
					}
				}
			}
			perturb >>= pySetPerturbBits
			i = int(uint(i)*5+1+perturb) & mask
		}
	}

	resize := func(minused int) {
		size := pySetMinSize
		for size <= minused {
			size <<= 1
		}
		newTable := make([]int, size)
		newUsed := make([]bool, size)
		for i, key := range table {
			if used[i] {
				insertClean(newTable, newUsed, key)
			}
		}
		table, used = newTable, newUsed
	}

	add := func(key int) {
		mask := len(table) - 1
		i := key & mask
		perturb := uint(key)
		for {
			probes := 0
			if i+pySetLinearProbe <= mask {
				probes = pySetLinearProbe
			}
			for j := 0; j <= probes; j++ {
				if !used[i+j] {
					table[i+j], used[i+j] = key, true
					fill++
					if fill*5 >= mask*3 {
						if fill > 50000 {
							resize(fill * 2)
						} else {
							resize(fill * 4)
						}
					}
					return
				}
				if table[i+j] == key {
					return
				}
			}
			perturb >>= pySetPerturbBits
			i = int(uint(i)*5+1+perturb) & mask
		}
	}

	for _, key := range entries {
		add(key)
	}
	out := make([]int, 0, fill)
	for i, key := range table {
		if used[i] {
			out = append(out, key)
		}
	}
	return out
}
