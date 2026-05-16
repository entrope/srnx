// SPDX-License-Identifier: MIT-Modern-Variant
package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

var (
	jobs       = flag.Int("j", 0, "number of concurrent workers (default: GOMAXPROCS)")
	toolPath   = flag.String("tool", "rnx2srnx_search", "path to rnx2srnx_search binary")
	tuplesFile = flag.String("tuples", "", "path to tuples file (required)")
	topN       = flag.Int("top", 10, "number of best tuples to print per corpus")
)

type job struct {
	corpus string // directory basename
	path   string // full file path
}

type result struct {
	corpus string
	raw    string // verbatim TSV line (file\ttuple\tbytes)
	tuple  string
	bytes  uint64
}

func worker(tool, tuples string, jobs <-chan job, results chan<- result, wg *sync.WaitGroup, filesCompleted *int64) {
	defer wg.Done()
	for j := range jobs {
		cmd := exec.Command(tool, "--tuples-file="+tuples, j.path)
		var stderr strings.Builder
		cmd.Stderr = &stderr
		out, err := cmd.Output()
		if err != nil {
			log.Printf("rnx2srnx_search failed for %s: %v\n%s", j.path, err, stderr.String())
			atomic.AddInt64(filesCompleted, 1)
			continue
		}
		if stderr.Len() > 0 {
			log.Printf("rnx2srnx_search stderr for %s:\n%s", j.path, stderr.String())
		}
		for _, line := range strings.Split(strings.TrimRight(string(out), "\n"), "\n") {
			if line == "" {
				continue
			}
			parts := strings.Split(line, "\t")
			if len(parts) != 3 {
				log.Printf("unexpected output line for %s: %q", j.path, line)
				continue
			}
			b, convErr := strconv.ParseUint(parts[2], 10, 64)
			if convErr != nil {
				log.Printf("bad byte count in line %q: %v", line, convErr)
				continue
			}
			results <- result{
				corpus: j.corpus,
				raw:    line,
				tuple:  parts[1],
				bytes:  b,
			}
		}
		atomic.AddInt64(filesCompleted, 1)
	}
}

func main() {
	log.SetFlags(0)
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: srnx_block_search [-j N] [-tool PATH] -tuples FILE [-top N] DIR [DIR...]\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	if *tuplesFile == "" {
		log.Fatal("-tuples is required")
	}
	if _, err := os.Stat(*tuplesFile); err != nil {
		log.Fatalf("tuples file: %v", err)
	}
	dirs := flag.Args()
	if len(dirs) == 0 {
		flag.Usage()
		os.Exit(1)
	}
	if *jobs <= 0 {
		*jobs = runtime.GOMAXPROCS(0)
	}

	// Open one TSV file per corpus dir up front so we fail fast.
	tsvFiles := make(map[string]*bufio.Writer, len(dirs))
	tsvClosers := make(map[string]*os.File, len(dirs))
	for _, dir := range dirs {
		base := filepath.Base(filepath.Clean(dir))
		name := base + ".tsv"
		f, err := os.Create(name)
		if err != nil {
			log.Fatalf("create %s: %v", name, err)
		}
		tsvFiles[base] = bufio.NewWriter(f)
		tsvClosers[base] = f
		log.Printf("output: %s", name)
	}

	// Pre-count total files so the progress display has a denominator.
	fileCounts := make(map[string]int)
	totalFiles := 0
	for _, dir := range dirs {
		base := filepath.Base(filepath.Clean(dir))
		entries, err := os.ReadDir(dir)
		if err != nil {
			log.Fatalf("readdir %s: %v", dir, err)
		}
		for _, ent := range entries {
			if ent.Type().IsRegular() {
				fileCounts[base]++
				totalFiles++
			}
		}
	}

	jobCh := make(chan job, 2**jobs)
	resultCh := make(chan result, 2**jobs)

	var filesCompleted int64

	// Progress display: print N/Total to stderr on a ticker.
	stopProgress := make(chan struct{})
	progressDone := make(chan struct{})
	go func() {
		defer close(progressDone)
		ticker := time.NewTicker(250 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				fmt.Fprintf(os.Stderr, "\r%d/%d", atomic.LoadInt64(&filesCompleted), totalFiles)
			case <-stopProgress:
				fmt.Fprintf(os.Stderr, "\r%d/%d\n", atomic.LoadInt64(&filesCompleted), totalFiles)
				return
			}
		}
	}()

	// Start workers.
	var wg sync.WaitGroup
	for ii := 0; ii < *jobs; ii++ {
		wg.Add(1)
		go worker(*toolPath, *tuplesFile, jobCh, resultCh, &wg, &filesCompleted)
	}

	// Collector: owns TSV writers and aggregation map.
	// corpus -> tuple -> total bytes
	totals := make(map[string]map[string]uint64)
	// corpus -> tuple count (distinct tuples seen)
	tupleSets := make(map[string]map[string]struct{})

	collectorDone := make(chan struct{})
	go func() {
		defer close(collectorDone)
		for r := range resultCh {
			w := tsvFiles[r.corpus]
			fmt.Fprintln(w, r.raw)

			if totals[r.corpus] == nil {
				totals[r.corpus] = make(map[string]uint64)
				tupleSets[r.corpus] = make(map[string]struct{})
			}
			totals[r.corpus][r.tuple] += r.bytes
			tupleSets[r.corpus][r.tuple] = struct{}{}
		}
	}()

	// Producer: walk dirs, emit jobs.
	for _, dir := range dirs {
		base := filepath.Base(filepath.Clean(dir))
		entries, err := os.ReadDir(dir)
		if err != nil {
			log.Fatalf("readdir %s: %v", dir, err)
		}
		for _, ent := range entries {
			if ent.Type().IsRegular() {
				jobCh <- job{corpus: base, path: filepath.Join(dir, ent.Name())}
			}
		}
	}
	close(jobCh)

	// Wait for workers, stop progress display, then drain collector.
	wg.Wait()
	close(stopProgress)
	<-progressDone
	close(resultCh)
	<-collectorDone

	// Flush and close TSV files.
	for base, w := range tsvFiles {
		if err := w.Flush(); err != nil {
			log.Printf("flush %s.tsv: %v", base, err)
		}
		tsvClosers[base].Close()
	}

	// Print per-corpus summary in input order.
	// Also accumulate cross-corpus totals.
	crossTotals := make(map[string]uint64)
	fmt.Println()
	for _, dir := range dirs {
		base := filepath.Base(filepath.Clean(dir))
		m := totals[base]
		if m == nil {
			fmt.Printf("# corpus: %s  (no results)\n\n", dir)
			continue
		}
		nTuples := len(tupleSets[base])
		fmt.Printf("# corpus: %s  (files=%d, tuples=%d)\n",
			dir, fileCounts[base], nTuples)

		type kv struct {
			tuple string
			bytes uint64
		}
		ranked := make([]kv, 0, len(m))
		for t, b := range m {
			ranked = append(ranked, kv{t, b})
			crossTotals[t] += b
		}
		sort.Slice(ranked, func(ii, jj int) bool {
			if ranked[ii].bytes != ranked[jj].bytes {
				return ranked[ii].bytes < ranked[jj].bytes
			}
			return ranked[ii].tuple < ranked[jj].tuple
		})
		shown := *topN
		if shown > len(ranked) {
			shown = len(ranked)
		}
		for ii := 0; ii < shown; ii++ {
			fmt.Printf("  %3d  %-30s  %d\n", ii+1, ranked[ii].tuple, ranked[ii].bytes)
		}
		fmt.Println()
	}

	// Cross-corpus aggregate (only if more than one corpus).
	if len(dirs) > 1 && len(crossTotals) > 0 {
		fmt.Printf("# aggregate across all corpora\n")
		type kv struct {
			tuple string
			bytes uint64
		}
		ranked := make([]kv, 0, len(crossTotals))
		for t, b := range crossTotals {
			ranked = append(ranked, kv{t, b})
		}
		sort.Slice(ranked, func(ii, jj int) bool {
			if ranked[ii].bytes != ranked[jj].bytes {
				return ranked[ii].bytes < ranked[jj].bytes
			}
			return ranked[ii].tuple < ranked[jj].tuple
		})
		shown := *topN
		if shown > len(ranked) {
			shown = len(ranked)
		}
		for ii := 0; ii < shown; ii++ {
			fmt.Printf("  %3d  %-30s  %d\n", ii+1, ranked[ii].tuple, ranked[ii].bytes)
		}
		fmt.Println()
	}
}
