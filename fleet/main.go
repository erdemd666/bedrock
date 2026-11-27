// bedrock-fleet collects the records of Bedrock clients booted over the network,
// shows them on a dashboard and serves the netboot files. It is meant for a
// trusted LAN: there is no login and no TLS (the client speaks plain HTTP).
package main

import (
	"encoding/csv"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"html/template"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	maxRecord = 1 << 20
	stamp     = "20060102T150405Z" // in every record id, after the serial
)

type verdictRow struct{ Test, Verdict, Detail string }

// record is the part of a Bedrock record the pages read (see record_json() in the OS tools).
type record struct {
	AssetTag string            `json:"asset_tag"`
	Facts    map[string]string `json:"facts"`
	Results  []verdictRow      `json:"results"`
}

// entry is one stored record as the dashboard lists it.
type entry struct {
	ID, Serial, Model, Tag, Result string
	Pass, Fail, Inc, Abs           int
	At                             time.Time
}

func (e entry) When() string { return e.At.Format("2006-01-02 15:04 UTC") }

type server struct {
	data, images string
	mu           sync.Mutex
	list         []entry // newest first
	byID         map[string]bool
}

var nonAlnum = regexp.MustCompile(`[^A-Za-z0-9]+`)

// safeName makes a serial usable in a file name and in a URL.
func safeName(s string) string {
	s = strings.Trim(nonAlnum.ReplaceAllString(s, "-"), "-")
	if len(s) > 40 {
		s = strings.Trim(s[:40], "-")
	}
	if s == "" {
		return "unknown"
	}
	return s
}

// usableSerial is false for the placeholders firmware ships when nobody wrote a real
// serial; such machines must not collapse into one row.
func usableSerial(s string) bool {
	l := strings.ToLower(s)
	for _, bad := range []string{"o.e.m", "default string", "not specified", "not applicable", "to be filled", "unknown", "serial number", "none", "0123456789"} {
		if strings.Contains(l, bad) {
			return false
		}
	}
	return len(strings.TrimSpace(s)) >= 4
}

func summarize(id string, rec record) entry {
	f := rec.Facts
	e := entry{ID: id, Serial: f["system.serial"], Tag: rec.AssetTag}
	maker, model := f["system.manufacturer"], f["system.product"]
	if model == "" {
		maker, model = f["board.manufacturer"], f["board.product"]
	}
	if model == "" {
		maker, model = "", f["cpu.brand"]
	}
	e.Model = strings.TrimSpace(model)
	if maker != "" && !strings.HasPrefix(strings.ToLower(model), strings.ToLower(maker)) {
		e.Model = strings.TrimSpace(maker + " " + model)
	}
	if e.Tag == "" {
		e.Tag = f["system.asset_tag"]
	}
	for _, r := range rec.Results {
		switch r.Verdict {
		case "PASS":
			e.Pass++
		case "FAIL":
			e.Fail++
		case "INCONCLUSIVE":
			e.Inc++
		default:
			e.Abs++
		}
	}
	e.Result = "NOT PRESENT"
	switch {
	case e.Fail > 0:
		e.Result = "FAIL"
	case e.Inc > 0:
		e.Result = "INCONCLUSIVE"
	case e.Pass > 0:
		e.Result = "PASS"
	}
	if len(id) >= len(stamp) {
		e.At, _ = time.Parse(stamp, id[len(id)-len(stamp):])
	}
	return e
}

// parse is the one place that decides what counts as a Bedrock record.
func parse(raw []byte) (record, error) {
	var rec record
	if err := json.Unmarshal(raw, &rec); err != nil {
		return rec, err
	}
	if rec.Facts == nil || rec.Results == nil {
		return rec, errors.New("missing facts or results")
	}
	return rec, nil
}

func newServer(data, images string) (*server, error) {
	if err := os.MkdirAll(data, 0o755); err != nil {
		return nil, err
	}
	s := &server{data: data, images: images, byID: map[string]bool{}}
	files, _ := filepath.Glob(filepath.Join(data, "*.json"))
	for _, p := range files {
		raw, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		if rec, err := parse(raw); err == nil {
			id := strings.TrimSuffix(filepath.Base(p), ".json")
			s.list = append(s.list, summarize(id, rec))
			s.byID[id] = true
		}
	}
	s.sort()
	return s, nil
}

func (s *server) sort() {
	sort.SliceStable(s.list, func(i, j int) bool { return s.list[i].At.After(s.list[j].At) })
}

// add stores the record exactly as received, under <serial>-<time>.json.
func (s *server) add(raw []byte, rec record, now time.Time) (entry, error) {
	serial := safeName(rec.Facts["system.serial"])
	s.mu.Lock()
	defer s.mu.Unlock()
	now = now.UTC().Truncate(time.Second)
	id := serial + "-" + now.Format(stamp)
	for s.byID[id] { // two records in the same second: the later one takes the next second
		now = now.Add(time.Second)
		id = serial + "-" + now.Format(stamp)
	}
	tmp := filepath.Join(s.data, ".incoming")
	if err := os.WriteFile(tmp, raw, 0o644); err != nil {
		return entry{}, err
	}
	if err := os.Rename(tmp, filepath.Join(s.data, id+".json")); err != nil {
		return entry{}, err
	}
	e := summarize(id, rec)
	s.byID[id] = true
	s.list = append(s.list, e)
	s.sort()
	return e, nil
}

// machines is the latest record of each machine, newest first.
func (s *server) machines() []entry {
	s.mu.Lock()
	defer s.mu.Unlock()
	seen := map[string]bool{}
	var out []entry
	for _, e := range s.list {
		if usableSerial(e.Serial) {
			if seen[e.Serial] {
				continue
			}
			seen[e.Serial] = true
		}
		out = append(out, e)
	}
	return out
}

// find returns the record with this id and the older records of the same machine.
func (s *server) find(id string) (e entry, earlier []entry, ok bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, x := range s.list {
		if x.ID == id {
			e, ok = x, true
		}
	}
	if ok && usableSerial(e.Serial) {
		for _, x := range s.list {
			if x.ID != id && x.Serial == e.Serial && x.At.Before(e.At) {
				earlier = append(earlier, x)
			}
		}
	}
	return
}

func (s *server) post(w http.ResponseWriter, r *http.Request) {
	raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxRecord))
	var tooBig *http.MaxBytesError
	if errors.As(err, &tooBig) {
		http.Error(w, "record larger than 1 MiB", http.StatusRequestEntityTooLarge)
		return
	}
	rec, perr := parse(raw)
	if err != nil || perr != nil {
		http.Error(w, "not a Bedrock record", http.StatusBadRequest)
		return
	}
	e, err := s.add(raw, rec, time.Now())
	if err != nil {
		log.Print(err)
		http.Error(w, "cannot store the record", http.StatusInternalServerError)
		return
	}
	log.Printf("record %s from %s: %s", e.ID, r.RemoteAddr, e.Result)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	fmt.Fprintf(w, "{\"id\":%q}\n", e.ID)
}

func (s *server) dashboard(w http.ResponseWriter, r *http.Request) {
	d := struct {
		Machines        []entry
		Pass, Inc, Fail int
	}{Machines: s.machines()}
	for _, e := range d.Machines {
		switch e.Result {
		case "PASS":
			d.Pass++
		case "INCONCLUSIVE":
			d.Inc++
		case "FAIL":
			d.Fail++
		}
	}
	render(w, "dashboard", d)
}

type kv struct{ K, V string }

func (s *server) machine(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	e, earlier, ok := s.find(id)
	var rec record
	var err error
	if ok { // id is a known entry, so it is letters, digits and dashes only
		var raw []byte
		if raw, err = os.ReadFile(filepath.Join(s.data, id+".json")); err == nil {
			rec, err = parse(raw)
		}
	}
	if !ok || err != nil {
		http.NotFound(w, r)
		return
	}
	d := struct {
		E       entry
		Facts   []kv
		Results []verdictRow
		Earlier []entry
	}{E: e, Results: rec.Results, Earlier: earlier}
	for k, v := range rec.Facts {
		d.Facts = append(d.Facts, kv{k, v})
	}
	sort.Slice(d.Facts, func(i, j int) bool { return d.Facts[i].K < d.Facts[j].K })
	render(w, "machine", d)
}

// cell stops spreadsheets from running a record's text as a formula.
func cell(s string) string {
	if s != "" && strings.ContainsRune("=+-@\t\r", rune(s[0])) {
		return "'" + s
	}
	return s
}

func (s *server) inventory(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Content-Disposition", `attachment; filename="inventory.csv"`)
	c := csv.NewWriter(w)
	c.Write([]string{"time", "model", "serial", "asset_tag", "result", "pass", "fail", "inconclusive", "not_present", "id"})
	for _, e := range s.machines() {
		c.Write([]string{e.At.Format(time.RFC3339), cell(e.Model), cell(e.Serial), cell(e.Tag), e.Result,
			strconv.Itoa(e.Pass), strconv.Itoa(e.Fail), strconv.Itoa(e.Inc), strconv.Itoa(e.Abs), e.ID})
	}
	c.Flush()
}

// The kernel line is the one on the stick's boot menu (os/board/bedrock/syslinux.cfg), plus the server.
// iPXE: buildarch is the firmware's bitness, cpuarch the CPU's (a 64-bit PC on BIOS runs 32-bit iPXE).
const ipxeScript = `#!ipxe
set arch i686
iseq ${buildarch} x86_64 && set arch x86_64 || goto cpu
goto boot
:cpu
iseq ${cpuarch} x86_64 && set arch x86_64 || goto boot
:boot
kernel %[1]s/boot/bzImage-${arch} quiet loglevel=0 vt.global_cursor_default=0 console=ttyS0,115200 console=tty0 bedrock.fleet=%[1]s
boot
`

func (s *server) bootScript(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	fmt.Fprintf(w, ipxeScript, "http://"+r.Host)
}

func (s *server) bootImage(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if name != "bzImage-x86_64" && name != "bzImage-i686" {
		http.NotFound(w, r)
		return
	}
	http.ServeFile(w, r, filepath.Join(s.images, name))
}

func (s *server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/records", s.post)
	mux.HandleFunc("GET /{$}", s.dashboard)
	mux.HandleFunc("GET /m/{id}", s.machine)
	mux.HandleFunc("GET /inventory.csv", s.inventory)
	mux.HandleFunc("GET /boot/boot.ipxe", s.bootScript)
	mux.HandleFunc("GET /boot/{name}", s.bootImage)
	return mux
}

func render(w http.ResponseWriter, name string, data any) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := pages.ExecuteTemplate(w, name, data); err != nil {
		log.Print(err)
	}
}

const style = `<style>
body{font:17px/1.5 Georgia,"Times New Roman",serif;color:#000;background:#fff;max-width:68rem;margin:2rem auto;padding:0 1rem}
.hdr{font-size:.8rem;letter-spacing:.14em;text-transform:uppercase;margin:0}
h1{font-size:1.6rem;font-weight:normal;margin:.2rem 0 1rem}
h2{font-size:1.1rem;margin:2rem 0 .3rem}
table{border-collapse:collapse;width:100%}
th,td{text-align:left;vertical-align:top;padding:.3rem .8rem .3rem 0;border-bottom:1px solid #000}
th{border-bottom:2px solid #000;font-weight:bold}
.n{text-align:right;font-variant-numeric:tabular-nums}
.FAIL{font-weight:bold;text-decoration:underline}
a{color:inherit}
</style>`

var pages = template.Must(template.New("").Parse(`
{{define "head"}}<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">` + style + `{{end}}
{{define "dashboard"}}{{template "head"}}<meta http-equiv="refresh" content="10"><title>Bedrock fleet</title>
<p class="hdr">BEDROCK · built by erdem6.com</p>
<h1>Fleet</h1>
{{if .Machines}}<p>{{len .Machines}} machines: {{.Pass}} pass, {{.Inc}} inconclusive, {{.Fail}} fail. <a href="/inventory.csv">Inventory (CSV)</a></p>
<table>
<tr><th>Time</th><th>Model</th><th>Serial</th><th>Asset tag</th><th>Result</th><th class="n">Pass</th><th class="n">Fail</th><th class="n">Inconcl.</th><th class="n">Absent</th></tr>
{{range .Machines}}<tr><td><a href="/m/{{.ID}}">{{.When}}</a></td><td>{{.Model}}</td><td>{{.Serial}}</td><td>{{.Tag}}</td><td class="{{.Result}}">{{.Result}}</td><td class="n">{{.Pass}}</td><td class="n">{{.Fail}}</td><td class="n">{{.Inc}}</td><td class="n">{{.Abs}}</td></tr>
{{end}}</table>
{{else}}<p>No machines yet. Boot a PC from the network and its record appears here.</p>{{end}}
{{end}}
{{define "machine"}}{{template "head"}}<title>Bedrock fleet</title>
<p class="hdr">BEDROCK · built by erdem6.com</p>
<h1>{{.E.Model}}</h1>
<p><a href="/">All machines</a></p>
<table>
<tr><th>Serial</th><td>{{.E.Serial}}</td></tr>
<tr><th>Asset tag</th><td>{{.E.Tag}}</td></tr>
<tr><th>Received</th><td>{{.E.When}}</td></tr>
<tr><th>Result</th><td class="{{.E.Result}}">{{.E.Result}}: {{.E.Pass}} pass, {{.E.Fail}} fail, {{.E.Inc}} inconclusive, {{.E.Abs}} not present</td></tr>
</table>
<h2>Results</h2>
<table>
{{range .Results}}<tr><td class="{{.Verdict}}">{{.Verdict}}</td><td>{{.Test}}</td><td>{{.Detail}}</td></tr>
{{end}}</table>
<h2>Facts</h2>
<table>
{{range .Facts}}<tr><td>{{.K}}</td><td>{{.V}}</td></tr>
{{end}}</table>
{{if .Earlier}}<h2>Earlier records of this machine</h2>
<p>{{range .Earlier}}<a href="/m/{{.ID}}">{{.When}}</a> {{.Result}}<br>{{end}}</p>
{{end}}
{{end}}`))

func main() {
	listen := flag.String("listen", ":8080", "address to listen on")
	data := flag.String("data", "data", "directory where records are stored")
	images := flag.String("images", ".", "directory holding bzImage-x86_64 and bzImage-i686")
	flag.Parse()
	s, err := newServer(*data, *images)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("bedrock-fleet on %s, %d records in %s", *listen, len(s.list), *data)
	srv := &http.Server{Addr: *listen, Handler: s.routes(), ReadHeaderTimeout: 10 * time.Second}
	log.Fatal(srv.ListenAndServe())
}
