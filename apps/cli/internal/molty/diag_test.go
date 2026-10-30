package molty

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"testing"
)

func TestNextSeq(t *testing.T) {
	for _, c := range []struct {
		text        string
		since, want int
	}{
		{"", 7, 7},
		{"3 0.100 a\n4 0.200 b\n", 0, 5},
		{"9 1.000 EVT x\n", 9, 10},
		{"garbage\n", 2, 2},
	} {
		if got := nextSeq(c.text, c.since); got != c.want {
			t.Errorf("nextSeq(%q,%d)=%d want %d", c.text, c.since, got, c.want)
		}
	}
}

func TestRelayDiagLogArgs(t *testing.T) {
	var got []string
	c := relayClient(t, func(w http.ResponseWriter, r *http.Request) {
		var cmd struct {
			Name string
			Args Result
		}
		_ = json.NewDecoder(r.Body).Decode(&cmd)
		b, _ := json.Marshal(cmd.Args)
		got = append(got, cmd.Name+string(b))
		fmt.Fprint(w, `{"ok":true,"result":{"next":5,"text":"4 0.1 x\n"}}`)
	})
	ctx := context.Background()
	if _, err := c.Diag(ctx, 60); err != nil {
		t.Fatal(err)
	}
	if _, err := c.Diag(ctx, 0); err != nil {
		t.Fatal(err)
	}
	if r, err := c.Log(ctx, 4, 2048); err != nil || r["next"].(float64) != 5 {
		t.Fatal(r, err)
	}
	want := []string{`diag{"timeline":60}`, `diag{}`, `log{"max":2048,"since":4}`}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Fatalf("got %v want %v", got, want)
	}
	if _, err := c.Diag(ctx, 181); err == nil {
		t.Fatal("timeline 181 accepted")
	}
	if _, err := c.Log(ctx, 0, 100); err == nil {
		t.Fatal("max 100 accepted")
	}
	if len(got) != 3 {
		t.Fatal("invalid args reached the relay")
	}
}
