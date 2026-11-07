package main

import (
	"fmt"
	"math"
	"reflect"
	"slices"
	"strconv"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

// Quote renders s as a JSON/C string literal with ensure_ascii escaping.
func Quote(s string) string {
	var b strings.Builder
	encodeString(&b, s, true)
	return b.String()
}

// QuoteUTF8 renders s as a JSON/C string literal that keeps non-ASCII runes.
func QuoteUTF8(s string) string {
	var b strings.Builder
	encodeString(&b, s, false)
	return b.String()
}

func encodeNewline(b *strings.Builder, indent, level int) {
	b.WriteByte('\n')
	b.WriteString(strings.Repeat(" ", indent*level))
}

func encodeOpen(b *strings.Builder, c byte, empty bool, indent, level int) {
	b.WriteByte(c)
	if !empty && indent > 0 {
		encodeNewline(b, indent, level+1)
	}
}

func encodeSeparator(b *strings.Builder, indent, level int) {
	if indent > 0 {
		b.WriteByte(',')
		encodeNewline(b, indent, level+1)
	} else {
		b.WriteString(", ")
	}
}

func encodeClose(b *strings.Builder, c byte, empty bool, indent, level int) {
	if !empty && indent > 0 {
		encodeNewline(b, indent, level)
	}
	b.WriteByte(c)
}

func encodeObject(b *strings.Builder, keys []string, get func(string) any, indent, level int, ascii bool) {
	encodeOpen(b, '{', len(keys) == 0, indent, level)
	for i, key := range keys {
		if i > 0 {
			encodeSeparator(b, indent, level)
		}
		encodeString(b, key, ascii)
		b.WriteString(": ")
		encodeValue(b, get(key), indent, level+1, ascii)
	}
	encodeClose(b, '}', len(keys) == 0, indent, level)
}

func encodeArray(b *strings.Builder, n int, get func(int) any, indent, level int, ascii bool) {
	encodeOpen(b, '[', n == 0, indent, level)
	for i := range n {
		if i > 0 {
			encodeSeparator(b, indent, level)
		}
		encodeValue(b, get(i), indent, level+1, ascii)
	}
	encodeClose(b, ']', n == 0, indent, level)
}

func encodeValue(b *strings.Builder, v any, indent, level int, ascii bool) {
	switch x := v.(type) {
	case nil:
		b.WriteString("null")
		return
	case bool:
		b.WriteString(strconv.FormatBool(x))
		return
	case string:
		encodeString(b, x, ascii)
		return
	case float64:
		encodeFloat(b, x)
		return
	case float32:
		encodeFloat(b, float64(x))
		return
	case *Fields:
		encodeObject(b, x.keys, func(k string) any { return x.values[k] }, indent, level, ascii)
		return
	case Fields:
		encodeObject(b, x.keys, func(k string) any { return x.values[k] }, indent, level, ascii)
		return
	}
	rv := reflect.ValueOf(v)
	switch rv.Kind() {
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		b.WriteString(strconv.FormatInt(rv.Int(), 10))
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		b.WriteString(strconv.FormatUint(rv.Uint(), 10))
	case reflect.String:
		encodeString(b, rv.String(), ascii)
	case reflect.Slice, reflect.Array:
		encodeArray(b, rv.Len(), func(i int) any { return rv.Index(i).Interface() }, indent, level, ascii)
	case reflect.Map:
		if rv.Type().Key().Kind() != reflect.String {
			panic(fmt.Sprintf("encode: unsupported map key type %s", rv.Type().Key()))
		}
		keys := make([]string, 0, rv.Len())
		for _, key := range rv.MapKeys() {
			keys = append(keys, key.String())
		}
		slices.Sort(keys)
		encodeObject(b, keys, func(k string) any {
			return rv.MapIndex(reflect.ValueOf(k).Convert(rv.Type().Key())).Interface()
		}, indent, level, ascii)
	case reflect.Pointer, reflect.Interface:
		if rv.IsNil() {
			b.WriteString("null")
			return
		}
		encodeValue(b, rv.Elem().Interface(), indent, level, ascii)
	default:
		panic(fmt.Sprintf("encode: unsupported type %T", v))
	}
}

func encodeFloat(b *strings.Builder, f float64) {
	switch {
	case math.IsNaN(f):
		b.WriteString("NaN")
	case math.IsInf(f, 1):
		b.WriteString("Infinity")
	case math.IsInf(f, -1):
		b.WriteString("-Infinity")
	default:
		b.WriteString(formatFloat(f))
	}
}

func encodeString(b *strings.Builder, s string, ascii bool) {
	b.WriteByte('"')
	for i := 0; i < len(s); {
		r, size := utf8.DecodeRuneInString(s[i:])
		i += size
		switch {
		case r == '"':
			b.WriteString(`\"`)
		case r == '\\':
			b.WriteString(`\\`)
		case r == '\n':
			b.WriteString(`\n`)
		case r == '\r':
			b.WriteString(`\r`)
		case r == '\t':
			b.WriteString(`\t`)
		case r == '\b':
			b.WriteString(`\b`)
		case r == '\f':
			b.WriteString(`\f`)
		case r < 0x20:
			fmt.Fprintf(b, `\u%04x`, r)
		case r < 0x7f || !ascii:
			b.WriteRune(r)
		case r >= 0x10000:
			hi, lo := utf16.EncodeRune(r)
			fmt.Fprintf(b, `\u%04x\u%04x`, hi, lo)
		default:
			fmt.Fprintf(b, `\u%04x`, r)
		}
	}
	b.WriteByte('"')
}
