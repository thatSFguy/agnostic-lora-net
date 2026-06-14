package httpd

import (
	"encoding/json"
	"os"
	"sync"
)

// uiState is the controller-side store for node aliases and map positions — the bits the
// map app kept in browser localStorage, moved server-side so they persist across browsers
// and machines and stay the single source of truth. Positions are relative coords in
// [0,1] (fraction of the map viewport) so they're resolution-independent.
type uiState struct {
	mu        sync.Mutex
	path      string
	Aliases   map[string]string    `json:"aliases"`
	Positions map[string][]float64 `json:"positions"` // id -> [x,y] in 0..1
}

func loadUI(path string) *uiState {
	u := &uiState{path: path, Aliases: map[string]string{}, Positions: map[string][]float64{}}
	if path != "" {
		if b, err := os.ReadFile(path); err == nil {
			_ = json.Unmarshal(b, u)
			if u.Aliases == nil {
				u.Aliases = map[string]string{}
			}
			if u.Positions == nil {
				u.Positions = map[string][]float64{}
			}
		}
	}
	return u
}

func (u *uiState) setAlias(id, name string) {
	u.mu.Lock()
	defer u.mu.Unlock()
	if name == "" {
		delete(u.Aliases, id)
	} else {
		u.Aliases[id] = name
	}
	u.save()
}

func (u *uiState) setPos(id string, x, y float64) {
	u.mu.Lock()
	defer u.mu.Unlock()
	u.Positions[id] = []float64{x, y}
	u.save()
}

// replaceAll overwrites aliases + positions wholesale (a backup restore) and persists.
func (u *uiState) replaceAll(aliases map[string]string, positions map[string][]float64) {
	u.mu.Lock()
	defer u.mu.Unlock()
	if aliases == nil {
		aliases = map[string]string{}
	}
	if positions == nil {
		positions = map[string][]float64{}
	}
	u.Aliases, u.Positions = aliases, positions
	u.save()
}

// snapshot returns copies safe to marshal without holding the lock.
func (u *uiState) snapshot() (map[string]string, map[string][]float64) {
	u.mu.Lock()
	defer u.mu.Unlock()
	a := make(map[string]string, len(u.Aliases))
	for k, v := range u.Aliases {
		a[k] = v
	}
	p := make(map[string][]float64, len(u.Positions))
	for k, v := range u.Positions {
		p[k] = v
	}
	return a, p
}

// save persists the state (called under lock). Best-effort; a UI-state write failure must
// never disrupt control.
func (u *uiState) save() {
	if u.path == "" {
		return
	}
	if b, err := json.MarshalIndent(struct {
		Aliases   map[string]string    `json:"aliases"`
		Positions map[string][]float64 `json:"positions"`
	}{u.Aliases, u.Positions}, "", "  "); err == nil {
		_ = os.WriteFile(u.path, b, 0o644)
	}
}
