// Generates the two fixture headers the library's tests compile in, so the
// tests are hermetic and still run against the formats we actually meet:
// libfriend/tests/fr_export_data.h is libfriend/tests/export.json as a C string, and
// libfriend/tests/fr_zip_data.h is real zip files dumped as C arrays, one of them that
// same export zipped the way the service zipped it.
//
//	cd libfriend/tests/gen_zip_fixtures
//	go run . ../export.json ../fr_export_data.h ../fr_zip_data.h
//
// Two writers, because the two shapes in the wild differ in the one place
// that matters to a streaming reader:
//
//   - archive/zip (what the service used) never knows the sizes in advance,
//     so it sets flag bit 3 and puts the CRC in a data descriptor after the
//     data. Every official export looks like this.
//   - a writer with a seekable output (zip(1), python's zipfile, most GUIs)
//     writes the CRC and sizes in the local header. That is what a user who
//     unzipped and re-zipped the JSON themselves will hand us.
package main

import (
	"archive/zip"
	"bytes"
	"compress/flate"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"os"
	"time"
)

// Streamed: flag bit 3, sizes and CRC in a trailing data descriptor.
func streamedZip(name string, data []byte, method uint16) []byte {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	w, err := zw.CreateHeader(&zip.FileHeader{
		Name:     name,
		Method:   method,
		Modified: time.Unix(1762000000, 0).UTC(),
	})
	if err != nil {
		panic(err)
	}
	if _, err := w.Write(data); err != nil {
		panic(err)
	}
	if err := zw.Close(); err != nil {
		panic(err)
	}
	return buf.Bytes()
}

// Seekable: no data descriptor, CRC and sizes in the local header.
func seekableZip(name string, data []byte, method uint16) []byte {
	payload := data
	if method == zip.Deflate {
		var c bytes.Buffer
		fw, err := flate.NewWriter(&c, flate.DefaultCompression)
		if err != nil {
			panic(err)
		}
		if _, err := fw.Write(data); err != nil {
			panic(err)
		}
		if err := fw.Close(); err != nil {
			panic(err)
		}
		payload = c.Bytes()
	}
	sum := crc32.ChecksumIEEE(data)

	var b bytes.Buffer
	put16 := func(v uint16) { binary.Write(&b, binary.LittleEndian, v) }
	put32 := func(v uint32) { binary.Write(&b, binary.LittleEndian, v) }

	put32(0x04034b50)
	put16(20) // version needed
	put16(0)  // flags: no descriptor
	put16(method)
	put16(0) // mod time
	put16(0) // mod date
	put32(sum)
	put32(uint32(len(payload)))
	put32(uint32(len(data)))
	put16(uint16(len(name)))
	put16(0)
	b.WriteString(name)
	localEnd := b.Len()
	b.Write(payload)

	// Central directory, so the file is a valid zip for other tools too.
	centralStart := b.Len()
	put32(0x02014b50)
	put16(20) // version made by
	put16(20) // version needed
	put16(0)
	put16(method)
	put16(0)
	put16(0)
	put32(sum)
	put32(uint32(len(payload)))
	put32(uint32(len(data)))
	put16(uint16(len(name)))
	put16(0)
	put16(0) // comment
	put16(0) // disk
	put16(0) // internal attrs
	put32(0) // external attrs
	put32(0) // offset of local header
	b.WriteString(name)
	centralSize := b.Len() - centralStart

	put32(0x06054b50)
	put16(0)
	put16(0)
	put16(1)
	put16(1)
	put32(uint32(centralSize))
	put32(uint32(centralStart))
	put16(0)
	_ = localEnd
	return b.Bytes()
}

// The 70 KB payload the C test recomputes rather than storing: long runs so
// matches reach back past the 32 KiB window boundary.
func pattern(n int) []byte {
	out := make([]byte, n)
	for i := range out {
		out[i] = byte('A' + ((i/101 + i%7) % 26))
	}
	return out
}

func dump(w *bytes.Buffer, name string, data []byte) {
	fmt.Fprintf(w, "static const unsigned char %s[] = {", name)
	for i, b := range data {
		if i%16 == 0 {
			fmt.Fprintf(w, "\n    ")
		}
		fmt.Fprintf(w, "0x%02x,", b)
	}
	fmt.Fprintf(w, "\n};\n\n")
}

// libfriend/tests/export.json as one C string literal, a line of JSON to a line of C.
// Anything outside printable ASCII is an octal escape, which is always
// exactly three digits and so cannot swallow the character after it.
func exportHeader(json []byte) []byte {
	var b bytes.Buffer
	b.WriteString(`/* fr_export_data.h — generated from libfriend/tests/export.json by
 * libfriend/tests/gen_zip_fixtures; edit the JSON and regenerate, not this.
 *
 * A real protojson FriendState, marshalled by the same code and options
 * the service's export RPC used. Frozen here so the importer is tested
 * against the wire format itself rather than a hand-written imitation of
 * it: every field name, every quoted 64-bit integer, every base64 blob
 * and every RFC 3339 timestamp is as it came out of the service.
 *
 * The embedding of memory k is byte j = (j * 31 + k * 17) % 256, byte i
 * of the photograph is (i * 3) % 256 and of its thumbnail (i * 7 + 1) %
 * 256, so the test can check every byte without the fixture stating them
 * twice.
 */

static const char fr_export_json[] =
`)
	for _, line := range bytes.SplitAfter(json, []byte("\n")) {
		if len(line) == 0 {
			continue
		}
		b.WriteString("    \"")
		for i, c := range line {
			switch {
			case c == '"' || c == '\\':
				b.WriteByte('\\')
				b.WriteByte(c)
			case c == '\n':
				b.WriteString(`\n`)
			case c == '?' && i > 0 && line[i-1] == '?':
				b.WriteString(`\?`) // not a trigraph
			case c >= 0x20 && c < 0x7f:
				b.WriteByte(c)
			default:
				fmt.Fprintf(&b, "\\%03o", c)
			}
		}
		b.WriteString("\"\n")
	}
	b.WriteString("    ;\n")
	return b.Bytes()
}

const smallJSON = `{"friendName":"Em","interiority":"I want to be told things first."}`

func main() {
	if len(os.Args) < 4 {
		fmt.Fprintln(os.Stderr, "usage: gen_zip_fixtures <export.json> <fr_export_data.h> <fr_zip_data.h>")
		os.Exit(2)
	}
	exportJSON, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	if err = os.WriteFile(os.Args[2], exportHeader(exportJSON), 0o644); err != nil {
		panic(err)
	}
	fmt.Fprintf(os.Stderr, "wrote %s\n", os.Args[2])

	var out bytes.Buffer
	out.WriteString(`/* fr_zip_data.h — generated, do not edit.
 *
 * Real zip files, as C arrays. The deflated ones come from Go's
 * archive/zip, which is what produced every export the service handed out.
 *
 *     cd libfriend/tests/gen_zip_fixtures
 *     go run . ../export.json ../fr_export_data.h ../fr_zip_data.h
 */

`)
	fmt.Fprintf(&out, "#define UZ_SMALL_JSON %q\n\n", smallJSON)
	dump(&out, "uz_streamed_deflate", streamedZip("friend_state.json", []byte(smallJSON), zip.Deflate))
	dump(&out, "uz_streamed_pattern", streamedZip("friend_state.json", pattern(70000), zip.Deflate))
	dump(&out, "uz_streamed_store", streamedZip("friend_state.json", []byte("stored by a writer that streamed it"), zip.Store))
	dump(&out, "uz_seekable_deflate", seekableZip("friend_state.json", []byte(smallJSON), zip.Deflate))
	dump(&out, "uz_seekable_store", seekableZip("friend_state.json", []byte(smallJSON), zip.Store))

	// The real thing: the export fixture, zipped the way the service
	// zipped it, so one test can run the whole path a user runs.
	dump(&out, "uz_export_zip", streamedZip("friend_state.json", exportJSON, zip.Deflate))

	if err = os.WriteFile(os.Args[3], out.Bytes(), 0o644); err != nil {
		panic(err)
	}
	fmt.Fprintf(os.Stderr, "wrote %s (%d bytes)\n", os.Args[3], out.Len())
}
