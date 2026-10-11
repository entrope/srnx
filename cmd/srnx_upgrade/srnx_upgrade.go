// SPDX-License-Identifier: MIT-Modern-Variant

// srnx_upgrade walks directory trees and re-encodes every *.srnx file in
// place with rnx2srnx, several files at a time.  rnx2srnx leaves files
// that are already the version it would write alone (exit status 2), so
// running this over a corpus only upgrades the older files, unless the
// tool arguments include --recompress.
package main

import (
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

var (
	jobs     = flag.Int("j", runtime.NumCPU(), "number of concurrent rnx2srnx processes")
	toolPath = flag.String("tool", "rnx2srnx", "path to rnx2srnx binary")
	toolArgs = flag.String("tool-args", "", "extra space-separated arguments for rnx2srnx (e.g. \"--recompress --check\")")
	verbose  = flag.Bool("v", false, "report every file, not just failures")
)

// exitAlreadyCurrent is rnx2srnx's exit status for SRNX input that is
// already the version it would write (or newer).
const exitAlreadyCurrent = 2

type outcome int

const (
	converted outcome = iota
	current
	failed
)

type result struct {
	path    string
	outcome outcome
	oldSize int64 // size before conversion
	newSize int64 // size after conversion, if converted
}

// reencode runs rnx2srnx on path, replacing it in place.
func reencode(tool string, args []string, path string) result {
	res := result{path: path, outcome: failed}
	info, err := os.Stat(path)
	if err != nil {
		log.Print(err)
		return res
	}
	res.oldSize = info.Size()

	// "--" keeps a path that starts with '-' from looking like an option.
	// Naming path as the output too makes rnx2srnx replace it, even when
	// it would not derive that output name itself (as for ".srnx").
	cmd := exec.Command(tool, append(append(append([]string{}, args...), "--"), path, path)...)
	var output strings.Builder
	cmd.Stdout = &output // with --check, rnxcmp reports differences here
	cmd.Stderr = &output
	err = cmd.Run()

	var exitErr *exec.ExitError
	switch {
	case err == nil:
		if info, err = os.Stat(path); err != nil {
			log.Print(err)
			return res
		}
		res.outcome = converted
		res.newSize = info.Size()
		if *verbose {
			fmt.Printf("%s: %d -> %d bytes\n", path, res.oldSize, res.newSize)
		}
	case errors.As(err, &exitErr) && exitErr.ExitCode() == exitAlreadyCurrent:
		res.outcome = current
		if *verbose {
			fmt.Printf("%s: already current\n", path)
		}
	default:
		log.Printf("%s: %s %v\n%s", path, tool, err, output.String())
	}
	return res
}

// walk sends the path of every regular file under root whose name ends
// in ".srnx" to paths, and returns the number of errors it reported.
// Symbolic links within root are not followed (or re-encoded), but root
// itself may be one.
func walk(root string, paths chan<- string) int {
	nErrors := 0
	if info, err := os.Stat(root); err == nil && info.IsDir() &&
		!strings.HasSuffix(root, string(filepath.Separator)) {
		root += string(filepath.Separator) // so WalkDir follows a symlink
	}
	filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			log.Print(err)
			nErrors++
			return nil
		}
		if d.Type().IsRegular() && strings.HasSuffix(d.Name(), ".srnx") {
			paths <- path
		}
		return nil
	})
	return nErrors
}

func main() {
	flag.Usage = func() {
		fmt.Fprintf(flag.CommandLine.Output(),
			"Usage: %s [options] [directory ...]\n"+
				"Re-encodes each *.srnx file under the directories (default: .) in place\n"+
				"with rnx2srnx, skipping files that are already its output version.\n",
			os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()
	log.SetFlags(0)
	log.SetPrefix("srnx_upgrade: ")
	if *jobs < 1 {
		log.Fatal("-j must be at least 1")
	}
	roots := flag.Args()
	if len(roots) == 0 {
		roots = []string{"."}
	}
	args := strings.Fields(*toolArgs)

	paths := make(chan string, 2**jobs)
	results := make(chan result, 2**jobs)
	var wg sync.WaitGroup
	for ii := 0; ii < *jobs; ii++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for path := range paths {
				results <- reencode(*toolPath, args, path)
			}
		}()
	}

	walkErrors := 0
	go func() {
		for _, root := range roots {
			walkErrors += walk(root, paths)
		}
		close(paths)
		wg.Wait()
		close(results)
	}()

	var nConverted, nCurrent, nFailed int
	var oldBytes, newBytes int64
	for res := range results {
		switch res.outcome {
		case converted:
			nConverted++
			oldBytes += res.oldSize
			newBytes += res.newSize
		case current:
			nCurrent++
		case failed:
			nFailed++
		}
	}

	fmt.Printf("%d converted", nConverted)
	if oldBytes > 0 {
		fmt.Printf(" (%d -> %d bytes, %+.2f%%)", oldBytes, newBytes,
			100*float64(newBytes-oldBytes)/float64(oldBytes))
	}
	fmt.Printf(", %d already current, %d failed", nCurrent, nFailed)
	if walkErrors > 0 {
		fmt.Printf(", %d directory walk errors", walkErrors)
	}
	fmt.Println()
	if nFailed > 0 || walkErrors > 0 {
		os.Exit(1)
	}
}
