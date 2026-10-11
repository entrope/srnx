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
	toolArgs   = flag.String("tool-args", "", "extra space-separated arguments for rnx2srnx_search (e.g. \"--soc2 --fixed-order\")")
	tuplesFile = flag.String("tuples", "", "path to tuples file (required unless -eliminate)")
	topN       = flag.Int("top", 10, "number of best tuples to print per corpus")
	sample     = flag.Int("sample", 1, "use every Nth file (by name) of each corpus")
	eliminate  = flag.String("eliminate", "", "comma-separated candidate block sizes: search by backward elimination from this set (must include 8)")
	keep       = flag.Int("keep", 7, "with -eliminate: number of block sizes to keep")
	swap       = flag.Bool("swap", true, "with -eliminate: refine the result with single-size swaps")
	baseline   = flag.String("baseline", "8,16,24,32,40,48,112", "with -eliminate: tuple to compare the result against")
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

// totals maps corpus -> tuple (dash-separated, as the tool prints it) -> bytes.
type totals map[string]map[string]uint64

func worker(tool string, args []string, jobs <-chan job, results chan<- result, wg *sync.WaitGroup, filesCompleted *int64) {
	defer wg.Done()
	for j := range jobs {
		cmd := exec.Command(tool, append(append([]string{}, args...), j.path)...)
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

// listFiles returns every sample'th regular file (sorted by name) of each
// corpus directory, and the number of files used per corpus.
func listFiles(dirs []string, sample int) ([]job, map[string]int) {
	var files []job
	counts := make(map[string]int)
	for _, dir := range dirs {
		base := filepath.Base(filepath.Clean(dir))
		entries, err := os.ReadDir(dir)
		if err != nil {
			log.Fatalf("readdir %s: %v", dir, err)
		}
		nn := 0
		for _, ent := range entries { // os.ReadDir sorts by name
			if !ent.Type().IsRegular() {
				continue
			}
			if nn%sample == 0 {
				files = append(files, job{corpus: base, path: filepath.Join(dir, ent.Name())})
				counts[base]++
			}
			nn++
		}
	}
	return files, counts
}

// evaluate runs the tool over files for the tuples in tuplesPath and
// returns per-corpus totals.  If tsv is non-nil, raw output lines are
// written to the writer for their corpus.
func evaluate(files []job, tuplesPath string, tsv map[string]*bufio.Writer) totals {
	args := append(strings.Fields(*toolArgs), "--tuples-file="+tuplesPath)
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
				fmt.Fprintf(os.Stderr, "\r%d/%d", atomic.LoadInt64(&filesCompleted), len(files))
			case <-stopProgress:
				fmt.Fprintf(os.Stderr, "\r%d/%d\n", atomic.LoadInt64(&filesCompleted), len(files))
				return
			}
		}
	}()

	var wg sync.WaitGroup
	for ii := 0; ii < *jobs; ii++ {
		wg.Add(1)
		go worker(*toolPath, args, jobCh, resultCh, &wg, &filesCompleted)
	}

	tot := make(totals)
	collectorDone := make(chan struct{})
	go func() {
		defer close(collectorDone)
		for r := range resultCh {
			if tsv != nil {
				fmt.Fprintln(tsv[r.corpus], r.raw)
			}
			if tot[r.corpus] == nil {
				tot[r.corpus] = make(map[string]uint64)
			}
			tot[r.corpus][r.tuple] += r.bytes
		}
	}()

	for _, f := range files {
		jobCh <- f
	}
	close(jobCh)
	wg.Wait()
	close(stopProgress)
	<-progressDone
	close(resultCh)
	<-collectorDone
	return tot
}

type kv struct {
	tuple string
	bytes uint64
}

func ranked(m map[string]uint64) []kv {
	out := make([]kv, 0, len(m))
	for t, b := range m {
		out = append(out, kv{t, b})
	}
	sort.Slice(out, func(ii, jj int) bool {
		if out[ii].bytes != out[jj].bytes {
			return out[ii].bytes < out[jj].bytes
		}
		return out[ii].tuple < out[jj].tuple
	})
	return out
}

func printTop(m map[string]uint64) {
	r := ranked(m)
	shown := *topN
	if shown > len(r) {
		shown = len(r)
	}
	for ii := 0; ii < shown; ii++ {
		fmt.Printf("  %3d  %-30s  %d\n", ii+1, r[ii].tuple, r[ii].bytes)
	}
	fmt.Println()
}

// runTuplesFile is the original mode: evaluate a fixed tuples file and
// rank the tuples per corpus and overall.
func runTuplesFile(dirs []string) {
	if _, err := os.Stat(*tuplesFile); err != nil {
		log.Fatalf("tuples file: %v", err)
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

	files, fileCounts := listFiles(dirs, *sample)
	tot := evaluate(files, *tuplesFile, tsvFiles)

	for base, w := range tsvFiles {
		if err := w.Flush(); err != nil {
			log.Printf("flush %s.tsv: %v", base, err)
		}
		tsvClosers[base].Close()
	}

	// Print per-corpus summary in input order, accumulating cross-corpus totals.
	crossTotals := make(map[string]uint64)
	fmt.Println()
	for _, dir := range dirs {
		base := filepath.Base(filepath.Clean(dir))
		m := tot[base]
		if m == nil {
			fmt.Printf("# corpus: %s  (no results)\n\n", dir)
			continue
		}
		fmt.Printf("# corpus: %s  (files=%d, tuples=%d)\n", dir, fileCounts[base], len(m))
		for t, b := range m {
			crossTotals[t] += b
		}
		printTop(m)
	}
	if len(dirs) > 1 && len(crossTotals) > 0 {
		fmt.Printf("# aggregate across all corpora\n")
		printTop(crossTotals)
	}
}

// ---- Backward elimination ----

func parseSizes(s string) []int {
	var out []int
	for _, f := range strings.Split(s, ",") {
		v, err := strconv.Atoi(strings.TrimSpace(f))
		if err != nil || v <= 0 || v%8 != 0 {
			log.Fatalf("bad block size %q in %q", f, s)
		}
		out = append(out, v)
	}
	sort.Ints(out)
	for ii := 1; ii < len(out); ii++ {
		if out[ii] == out[ii-1] {
			log.Fatalf("duplicate block size %d in %q", out[ii], s)
		}
	}
	if len(out) == 0 || out[0] != 8 {
		log.Fatalf("block sizes %q must include 8", s)
	}
	return out
}

func joinSizes(sizes []int, sep string) string {
	parts := make([]string, len(sizes))
	for ii, v := range sizes {
		parts[ii] = strconv.Itoa(v)
	}
	return strings.Join(parts, sep)
}

func without(sizes []int, drop int) []int {
	out := make([]int, 0, len(sizes))
	for _, v := range sizes {
		if v != drop {
			out = append(out, v)
		}
	}
	return out
}

func with(sizes []int, add int) []int {
	out := append(append([]int{}, sizes...), add)
	sort.Ints(out)
	return out
}

func contains(sizes []int, v int) bool {
	for _, s := range sizes {
		if s == v {
			return true
		}
	}
	return false
}

// evalSets evaluates the given sets (the first one is the reference for
// --fixed-order) and returns the total over all corpora for each set,
// plus the per-corpus totals.
func evalSets(files []job, sets [][]int) (map[string]uint64, totals) {
	f, err := os.CreateTemp("", "srnx_block_search-*.tuples")
	if err != nil {
		log.Fatalf("create tuples file: %v", err)
	}
	defer os.Remove(f.Name())
	w := bufio.NewWriter(f)
	for _, set := range sets {
		fmt.Fprintln(w, joinSizes(set, ","))
	}
	if err := w.Flush(); err != nil {
		log.Fatalf("write tuples file: %v", err)
	}
	f.Close()

	tot := evaluate(files, f.Name(), nil)
	all := make(map[string]uint64)
	for _, m := range tot {
		for t, b := range m {
			all[t] += b
		}
	}
	return all, tot
}

func runEliminate(dirs []string) {
	files, counts := listFiles(dirs, *sample)
	for _, dir := range dirs {
		base := filepath.Base(filepath.Clean(dir))
		log.Printf("corpus %s: %d files", base, counts[base])
	}
	candidates := parseSizes(*eliminate)
	base := parseSizes(*baseline)
	cur := candidates
	if *keep < 1 || *keep > len(candidates) {
		log.Fatalf("-keep=%d out of range for %d candidates", *keep, len(candidates))
	}

	// Backward elimination: drop the size whose removal costs least.
	for len(cur) > *keep {
		sets := [][]int{cur}
		for _, v := range cur {
			if v != 8 {
				sets = append(sets, without(cur, v))
			}
		}
		all, _ := evalSets(files, sets)
		curBytes := all[joinSizes(cur, "-")]
		type pen struct {
			size  int
			bytes uint64
		}
		var pens []pen
		for _, v := range cur {
			if v != 8 {
				pens = append(pens, pen{v, all[joinSizes(without(cur, v), "-")]})
			}
		}
		sort.Slice(pens, func(ii, jj int) bool { return pens[ii].bytes < pens[jj].bytes })
		fmt.Printf("# %d sizes: %s  %d bytes\n", len(cur), joinSizes(cur, "-"), curBytes)
		fmt.Printf("#   removal penalty (bytes, %%):")
		for _, p := range pens {
			fmt.Printf(" %d:%+d(%+.4f%%)", p.size, int64(p.bytes)-int64(curBytes),
				100*(float64(p.bytes)/float64(curBytes)-1))
		}
		fmt.Printf("\n#   remove %d\n", pens[0].size)
		cur = without(cur, pens[0].size)
	}

	// Single-swap refinement: replace one size with an unused candidate.
	for *swap {
		sets := [][]int{cur}
		for _, out := range cur {
			if out == 8 {
				continue
			}
			for _, in := range candidates {
				if !contains(cur, in) {
					sets = append(sets, with(without(cur, out), in))
				}
			}
		}
		if len(sets) == 1 {
			break
		}
		all, _ := evalSets(files, sets)
		curBytes := all[joinSizes(cur, "-")]
		best := ranked(all)[0]
		fmt.Printf("# swap round: %s  %d bytes; best %s  %d bytes\n",
			joinSizes(cur, "-"), curBytes, best.tuple, best.bytes)
		if best.bytes >= curBytes {
			break
		}
		cur = parseSizes(strings.ReplaceAll(best.tuple, "-", ","))
	}

	// Final comparison against the baseline, per corpus.  The result goes
	// first so it is the --fixed-order reference.
	all, tot := evalSets(files, [][]int{cur, base})
	resKey, baseKey := joinSizes(cur, "-"), joinSizes(base, "-")
	fmt.Printf("# result   %-40s %d bytes\n", resKey, all[resKey])
	fmt.Printf("# baseline %-40s %d bytes (result %+.4f%%)\n", baseKey, all[baseKey],
		100*(float64(all[resKey])/float64(all[baseKey])-1))
	for _, dir := range dirs {
		b := filepath.Base(filepath.Clean(dir))
		if m := tot[b]; m != nil && m[baseKey] > 0 {
			fmt.Printf("#   %-20s result %d  baseline %d  (%+.4f%%)\n", b, m[resKey], m[baseKey],
				100*(float64(m[resKey])/float64(m[baseKey])-1))
		}
	}
}

func main() {
	log.SetFlags(0)
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: srnx_block_search [-j N] [-tool PATH] [-tool-args ARGS] [-sample N]\n"+
			"         (-tuples FILE [-top N] | -eliminate SIZES [-keep N] [-swap=BOOL] [-baseline SIZES])\n"+
			"         DIR [DIR...]\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	dirs := flag.Args()
	if len(dirs) == 0 {
		flag.Usage()
		os.Exit(1)
	}
	if *jobs <= 0 {
		*jobs = runtime.GOMAXPROCS(0)
	}
	if *sample < 1 {
		log.Fatal("-sample must be at least 1")
	}

	switch {
	case *eliminate != "":
		runEliminate(dirs)
	case *tuplesFile != "":
		runTuplesFile(dirs)
	default:
		log.Fatal("-tuples or -eliminate is required")
	}
}
