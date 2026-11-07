package main

import (
	"fmt"
	"math"
	"reflect"
	"slices"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

type ordered interface {
	Keys() []string
	Get(key string) (any, bool)
}

func Format(v any) string {
	switch x := v.(type) {
	case nil:
		return "None"
	case bool:
		if x {
			return "True"
		}
		return "False"
	case string:
		return stringRepr(x)
	case float64:
		return formatFloat(x)
	case float32:
		return formatFloat(float64(x))
	case ordered:
		keys := x.Keys()
		parts := make([]string, len(keys))
		for i, k := range keys {
			item, _ := x.Get(k)
			parts[i] = stringRepr(k) + ": " + Format(item)
		}
		return "{" + strings.Join(parts, ", ") + "}"
	}
	rv := reflect.ValueOf(v)
	switch rv.Kind() {
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return strconv.FormatInt(rv.Int(), 10)
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		return strconv.FormatUint(rv.Uint(), 10)
	case reflect.Slice, reflect.Array:
		parts := make([]string, rv.Len())
		for i := range parts {
			parts[i] = Format(rv.Index(i).Interface())
		}
		return "[" + strings.Join(parts, ", ") + "]"
	case reflect.Map:
		keys := rv.MapKeys()
		slices.SortFunc(keys, func(a, b reflect.Value) int { return strings.Compare(fmt.Sprint(a), fmt.Sprint(b)) })
		parts := make([]string, len(keys))
		for i, k := range keys {
			parts[i] = Format(k.Interface()) + ": " + Format(rv.MapIndex(k).Interface())
		}
		return "{" + strings.Join(parts, ", ") + "}"
	}
	return fmt.Sprint(v)
}

func Text(v any) string {
	if s, ok := v.(string); ok {
		return s
	}
	return Format(v)
}

func stringRepr(s string) string {
	quote := '\''
	if strings.ContainsRune(s, '\'') && !strings.ContainsRune(s, '"') {
		quote = '"'
	}
	var b strings.Builder
	b.WriteRune(quote)
	for i := 0; i < len(s); {
		r, size := utf8.DecodeRuneInString(s[i:])
		if r == utf8.RuneError && size == 1 {
			fmt.Fprintf(&b, "\\x%02x", s[i])
			i++
			continue
		}
		i += size
		switch {
		case r == quote || r == '\\':
			b.WriteByte('\\')
			b.WriteRune(r)
		case r == '\t':
			b.WriteString(`\t`)
		case r == '\n':
			b.WriteString(`\n`)
		case r == '\r':
			b.WriteString(`\r`)
		case unicode.IsPrint(r):
			b.WriteRune(r)
		case r < 0x100:
			fmt.Fprintf(&b, "\\x%02x", r)
		case r < 0x10000:
			fmt.Fprintf(&b, "\\u%04x", r)
		default:
			fmt.Fprintf(&b, "\\U%08x", r)
		}
	}
	b.WriteRune(quote)
	return b.String()
}

// formatFloat is Python's repr(float), which the generated JSON depends on.
func formatFloat(f float64) string {
	switch {
	case math.IsNaN(f):
		return "nan"
	case math.IsInf(f, 1):
		return "inf"
	case math.IsInf(f, -1):
		return "-inf"
	}
	sci := strconv.FormatFloat(f, 'e', -1, 64)
	mantissa, expText, _ := strings.Cut(sci, "e")
	exp, _ := strconv.Atoi(expText)
	sign := ""
	if mantissa[0] == '-' {
		sign, mantissa = "-", mantissa[1:]
	}
	digits := strings.Replace(mantissa, ".", "", 1)
	decpt := exp + 1
	switch {
	case decpt > 16 || decpt < -3:
		out := digits[:1]
		if len(digits) > 1 {
			out += "." + digits[1:]
		}
		expSign := "+"
		if exp < 0 {
			expSign, exp = "-", -exp
		}
		return fmt.Sprintf("%s%se%s%02d", sign, out, expSign, exp)
	case decpt <= 0:
		return sign + "0." + strings.Repeat("0", -decpt) + digits
	case decpt >= len(digits):
		return sign + digits + strings.Repeat("0", decpt-len(digits)) + ".0"
	default:
		return sign + digits[:decpt] + "." + digits[decpt:]
	}
}

// ParseNumber parses an integer literal with Python's int(text, base) rules for
// base 10 and 16: surrounding whitespace, one optional sign, an optional 0x
// prefix (base 16) and single underscores between digits are accepted.
func ParseNumber(text string, base int) (int, error) {
	fail := fmt.Errorf("invalid literal for int() with base %d: %s", base, stringRepr(text))
	s := strings.TrimSpace(text)
	negative := false
	if s != "" && (s[0] == '+' || s[0] == '-') {
		negative = s[0] == '-'
		s = s[1:]
	}
	if base == 16 && len(s) > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') {
		s = s[2:]
		s = strings.TrimPrefix(s, "_")
	}
	if s == "" || s[0] == '_' || s[len(s)-1] == '_' || strings.Contains(s, "__") {
		return 0, fail
	}
	value, err := strconv.ParseInt(strings.ReplaceAll(s, "_", ""), base, 64)
	if err != nil || strings.ContainsAny(s, "+-") {
		return 0, fail
	}
	if negative {
		value = -value
	}
	return int(value), nil
}

func intOf(v any) (int, error) {
	if n, ok := pyIntValue(v); ok {
		return n, nil
	}
	switch x := v.(type) {
	case float64:
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return 0, fmt.Errorf("cannot convert float %s to integer", Format(x))
		}
		return int(x), nil
	case string:
		return ParseNumber(x, 10)
	}
	return 0, fmt.Errorf("int() argument must be a string, a bytes-like object or a real number, not %s", typeName(v))
}

func pyIntValue(v any) (int, bool) {
	if b, ok := v.(bool); ok {
		if b {
			return 1, true
		}
		return 0, true
	}
	return intValue(v)
}

func typeName(v any) string {
	switch v.(type) {
	case nil:
		return "'NoneType'"
	case map[string]any:
		return "'dict'"
	}
	if _, ok := asList(v); ok {
		return "'list'"
	}
	return "'object'"
}
