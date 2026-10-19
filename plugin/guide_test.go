package plugin

import (
	"os"
	"strings"
	"testing"
)

func TestGameFileGuideAllPlatforms(t *testing.T) {
	for _, game := range []struct{ label, folder string }{{"Undertale 1.08", "undertale"}, {"Deltarune Chapter 1", "deltarune"}} {
		for _, platform := range []string{"macOS", "Windows", "Linux"} {
			t.Run(game.folder+"/"+platform, func(t *testing.T) {
				root := t.TempDir()
				s := New(root, nil)
				confirm := selectGame(t, s, game.label)
				next := func(m menu, value string) menu {
					t.Helper()
					b, e := s.Handle(answer(m.Choice.Name, value))
					return readMenu(t, b, e)
				}
				systems := next(confirm, "Find my game files")
				page := next(systems, platform)
				for i := 0; i < 3; i++ {
					if len(page.Choice.Query) > 210 {
						t.Fatalf("long guide step: %s", page.Choice.Query)
					}
					if i == 0 && !strings.Contains(page.Choice.Query, "Browse local files") {
						t.Fatal(page.Choice.Query)
					}
					if i == 1 && platform == "macOS" && !strings.Contains(page.Choice.Query, "Show Package Contents") {
						t.Fatal(page.Choice.Query)
					}
					if i == 1 && game.folder == "deltarune" {
						chapter := map[string]string{"macOS": "chapter1_mac", "Windows": "chapter1_windows", "Linux": "chapter1_linux"}[platform]
						if !strings.Contains(page.Choice.Query, chapter) || !strings.Contains(page.Choice.Query, "mus") {
							t.Fatal(page.Choice.Query)
						}
					}
					if i < 2 {
						page = next(page, "Next")
					}
				}
				if !strings.Contains(page.Choice.Query, "Assets/butterscotch/common/"+game.folder) {
					t.Fatal(page.Choice.Query)
				}
				previous := next(page, "Back")
				page = next(previous, "Next")
				confirm = next(page, "Ready to install")
				if confirm.Choice.Choices[0] != "Install and keep originals" {
					t.Fatal(confirm)
				}
				// Reopening the guide remembers the OS but restarts at the first step.
				first := next(confirm, "Find my game files")
				if !strings.Contains(first.Choice.Query, "1/3") {
					t.Fatal(first.Choice.Query)
				}
				systems = next(first, "Back")
				confirm = next(systems, "Back")
				if confirm.Choice.Choices[0] != "Install and keep originals" {
					t.Fatal(confirm)
				}
				entries, e := os.ReadDir(root)
				if e != nil || len(entries) != 0 {
					t.Fatal("guide wrote files", e)
				}
			})
		}
	}
}

func TestGuideRejectsInstallAndStaleAnswers(t *testing.T) {
	s := New(t.TempDir(), nil)
	confirm := selectGame(t, s, "Undertale 1.08")
	b, e := s.Handle(answer(confirm.Choice.Name, "Find my game files"))
	systems := readMenu(t, b, e)
	if _, e := s.Handle(answer(systems.Choice.Name, "Install and discard originals")); e == nil {
		t.Fatal("guide accepted installation")
	}
	if _, e := s.Handle(answer(confirm.Choice.Name, "Install and keep originals")); e == nil {
		t.Fatal("stale confirmation accepted")
	}
	b, e = s.Handle([]byte(`"Kill"`))
	if e != nil || string(b) != `"Exit"` {
		t.Fatalf("kill: %s %v", b, e)
	}
}
