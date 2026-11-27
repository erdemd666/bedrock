package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const rec1 = `{"tool":{"name":"Bedrock"},"created":"2026-01-01T00:00:00Z","asset_tag":"A-17",
"facts":{"system.manufacturer":"Dell Inc.","system.product":"OptiPlex 7010","system.serial":"SN/42 <b>"},
"results":[{"test":"CPU","verdict":"PASS","detail":"ok"},{"test":"Memory","verdict":"FAIL","detail":"3 errors"},
{"test":"Fan","verdict":"INCONCLUSIVE","detail":"x"},{"test":"GPU","verdict":"NOT PRESENT","detail":"-"}],
"summary":{"pass":1,"fail":1,"inconclusive":1,"not_present":1}}`

func setup(t *testing.T) (*server, http.Handler) {
	t.Helper()
	s, err := newServer(filepath.Join(t.TempDir(), "data"), t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	return s, s.routes()
}

func do(h http.Handler, method, path, body string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, strings.NewReader(body))
	r.Host = "10.0.0.5:8080"
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}

func TestPostStoresRecord(t *testing.T) {
	s, h := setup(t)
	w := do(h, "POST", "/api/records", rec1)
	if w.Code != 201 {
		t.Fatalf("status %d: %s", w.Code, w.Body)
	}
	files, _ := filepath.Glob(filepath.Join(s.data, "SN-42-b-*.json"))
	if len(files) != 1 {
		t.Fatalf("want one file named after the serial, got %v", files)
	}
	if got, _ := os.ReadFile(files[0]); string(got) != rec1 {
		t.Error("record not stored verbatim")
	}
	// Same second, same serial: a second file, not an overwrite.
	for i := 0; i < 3; i++ {
		do(h, "POST", "/api/records", rec1)
	}
	if files, _ = filepath.Glob(filepath.Join(s.data, "*.json")); len(files) != 4 {
		t.Errorf("want 4 files, got %d", len(files))
	}
}

func TestPostRejectsBadInput(t *testing.T) {
	_, h := setup(t)
	for name, body := range map[string]string{
		"not json":      "hello",
		"empty":         "",
		"null":          "null",
		"no facts":      `{"results":[]}`,
		"no results":    `{"facts":{}}`,
		"numeric fact":  `{"facts":{"a":1},"results":[]}`,
		"trailing data": `{"facts":{},"results":[]} x`,
	} {
		if w := do(h, "POST", "/api/records", body); w.Code != 400 {
			t.Errorf("%s: status %d, want 400", name, w.Code)
		}
	}
	big := `{"facts":{"a":"` + strings.Repeat("x", maxRecord) + `"},"results":[]}`
	if w := do(h, "POST", "/api/records", big); w.Code != 413 {
		t.Errorf("oversize: status %d, want 413", w.Code)
	}
	if w := do(h, "GET", "/api/records", ""); w.Code != 405 {
		t.Errorf("GET /api/records: status %d, want 405", w.Code)
	}
}

func TestDashboardAndDetails(t *testing.T) {
	_, h := setup(t)
	if w := do(h, "GET", "/", ""); !strings.Contains(w.Body.String(), "No machines yet") {
		t.Error("empty dashboard should say so")
	}
	id := strings.Split(do(h, "POST", "/api/records", rec1).Body.String(), `"`)[3]
	w := do(h, "GET", "/", "")
	for _, want := range []string{"Dell Inc. OptiPlex 7010", "SN/42 &lt;b&gt;", "A-17", ">FAIL<", `http-equiv="refresh"`, "BEDROCK · built by erdem6.com"} {
		if !strings.Contains(w.Body.String(), want) {
			t.Errorf("dashboard lacks %q", want)
		}
	}
	if strings.Contains(w.Body.String(), "<b>") {
		t.Error("serial was not escaped")
	}
	w = do(h, "GET", "/m/"+id, "")
	if w.Code != 200 || !strings.Contains(w.Body.String(), "3 errors") || !strings.Contains(w.Body.String(), "system.serial") {
		t.Errorf("details page: %d %s", w.Code, w.Body)
	}
	for _, bad := range []string{"/m/nope", "/m/..", "/m/%2e%2e%2f%2e%2e%2fetc%2fpasswd", "/m/" + id + ".json"} {
		if w := do(h, "GET", bad, ""); w.Code == 200 { // the mux may redirect a cleaned path; it must never serve it
			t.Errorf("%s: status %d, want not served", bad, w.Code)
		}
	}
}

func TestLatestPerMachine(t *testing.T) {
	s, h := setup(t)
	pass := strings.Replace(rec1, `"verdict":"FAIL"`, `"verdict":"PASS"`, 1)
	s.add([]byte(rec1), mustParse(t, rec1), mustTime("2026-03-01T10:00:00Z"))
	s.add([]byte(pass), mustParse(t, pass), mustTime("2026-03-02T10:00:00Z"))
	if m := s.machines(); len(m) != 1 || m[0].Fail != 0 {
		t.Fatalf("want the newer record only, got %+v", m)
	}
	// Placeholder serials never merge.
	blank := `{"facts":{"system.serial":"To Be Filled By O.E.M."},"results":[]}`
	s.add([]byte(blank), mustParse(t, blank), mustTime("2026-03-03T10:00:00Z"))
	s.add([]byte(blank), mustParse(t, blank), mustTime("2026-03-04T10:00:00Z"))
	if m := s.machines(); len(m) != 3 {
		t.Fatalf("want 3 machines, got %d", len(m))
	}
	id := s.machines()[2].ID
	if w := do(h, "GET", "/m/"+id, ""); !strings.Contains(w.Body.String(), "Earlier records") {
		t.Error("details should list the earlier run")
	}
}

func TestRestartKeepsRecords(t *testing.T) {
	s, h := setup(t)
	do(h, "POST", "/api/records", rec1)
	s2, err := newServer(s.data, s.images)
	if err != nil || len(s2.machines()) != 1 {
		t.Fatalf("reloaded server lost the record: %v %v", err, s2.machines())
	}
}

func TestInventoryCSV(t *testing.T) {
	_, h := setup(t)
	do(h, "POST", "/api/records", strings.Replace(rec1, "SN/42 <b>", "=cmd|' /c calc'!A1", 1))
	w := do(h, "GET", "/inventory.csv", "")
	lines := strings.Split(strings.TrimSpace(w.Body.String()), "\n")
	if w.Code != 200 || len(lines) != 2 || !strings.HasPrefix(lines[0], "time,model,serial,asset_tag,result,") {
		t.Fatalf("csv: %d %q", w.Code, w.Body)
	}
	if !strings.Contains(lines[1], "Dell Inc. OptiPlex 7010,'=cmd") || !strings.Contains(lines[1], ",A-17,FAIL,1,1,1,1,") {
		t.Errorf("csv row: %s", lines[1])
	}
}

func TestBootScriptAndImages(t *testing.T) {
	s, h := setup(t)
	w := do(h, "GET", "/boot/boot.ipxe", "")
	body := w.Body.String()
	for _, want := range []string{"#!ipxe", "${buildarch}", "http://10.0.0.5:8080/boot/bzImage-${arch}", "bedrock.fleet=http://10.0.0.5:8080\n",
		"quiet loglevel=0 vt.global_cursor_default=0 console=ttyS0,115200 console=tty0"} {
		if !strings.Contains(body, want) {
			t.Errorf("boot script lacks %q:\n%s", want, body)
		}
	}
	os.WriteFile(filepath.Join(s.images, "bzImage-x86_64"), []byte("KERNEL"), 0o644)
	if w := do(h, "GET", "/boot/bzImage-x86_64", ""); w.Code != 200 || w.Body.String() != "KERNEL" {
		t.Errorf("kernel: %d %q", w.Code, w.Body)
	}
	if w := do(h, "GET", "/boot/bzImage-i686", ""); w.Code != 404 {
		t.Errorf("missing kernel: %d, want 404", w.Code)
	}
	os.WriteFile(filepath.Join(s.images, "secret"), []byte("x"), 0o644)
	if w := do(h, "GET", "/boot/secret", ""); w.Code != 404 {
		t.Errorf("only the two kernels are served, got %d", w.Code)
	}
}

func mustParse(t *testing.T, s string) record {
	t.Helper()
	r, err := parse([]byte(s))
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func mustTime(s string) time.Time {
	t, err := time.Parse(time.RFC3339, s)
	if err != nil {
		panic(err)
	}
	return t
}
