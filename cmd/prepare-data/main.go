package main

import (
	"flag"
	"fmt"
	"github.com/zenibako/butterscotch-pocket/prep"
	"io"
	"os"
)

func run(args []string, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("prepare-data", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var o prep.Options
	fs.StringVar(&o.Game, "game", "", "undertale or deltarune")
	fs.StringVar(&o.Source, "source", "", "installed game folder, .app, chapter folder or data file")
	fs.StringVar(&o.Output, "output", "", "new output folder outside the game installation")
	if e := fs.Parse(args); e != nil {
		if e == flag.ErrHelp {
			return 0
		}
		return 2
	}
	if o.Game == "" || o.Source == "" || o.Output == "" || fs.NArg() != 0 {
		fmt.Fprintln(stderr, "--game, --source and --output are required; no positional arguments are accepted")
		return 2
	}
	o.Progress = func(s string) { fmt.Fprintln(stdout, s) }
	out, e := prep.Prepare(o)
	if e != nil {
		fmt.Fprintln(stderr, "Preparation failed:", e)
		return 1
	}
	fmt.Fprintf(stdout, "Ready: %s\nMerge its Assets folder into the root of your Pocket SD card, then unmount the card.\nKeep these generated game files private. Your original files and saves were not changed.\n", out)
	return 0
}
func main() { os.Exit(run(os.Args[1:], os.Stdout, os.Stderr)) }
