package prep

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// InstallOptions selects original files directly in Assets/butterscotch/common.
// DiscardOriginals removes only consumed, unchanged support files and music
// claimed by this game, preserving required data files and shared or unknown inputs.
type InstallOptions struct {
	Root, Game       string
	DiscardOriginals bool
	Progress         func(string)
}

// InstallResult distinguishes a completed installation from optional cleanup.
type InstallResult struct {
	OriginalsDiscarded bool
	AlreadyInstalled   bool
	Warning            string
}

// Install converts a fixed card-local source and replaces only the selected
// game's files. Checked renames roll back ordinary failures; this is not a
// multi-file atomic transaction in the event of power loss or card removal.
func Install(o InstallOptions) (InstallResult, error) {
	return installWithCleanup(o, os.Rename, os.Remove, removeTree)
}

// installWithOps keeps fault injection local to a call, without global hooks.
func installWithOps(o InstallOptions, rename func(string, string) error, discard func(string) error) (InstallResult, error) {
	return installWithCleanup(o, rename, discard, removeTree)
}

func installWithCleanup(o InstallOptions, rename func(string, string) error, discard, cleanup func(string) error) (result InstallResult, err error) {
	if o.Game != "undertale" && o.Game != "deltarune" {
		return result, fmt.Errorf("choose undertale or deltarune")
	}
	if o.Root == "" {
		return result, fmt.Errorf("card root is required")
	}
	root, err := canonicalPath(o.Root)
	if err != nil {
		return result, err
	}
	common := filepath.Join(root, "Assets/butterscotch/common")
	names := []string{"data.win", "textures.bin", "music.bin"}
	if o.Game == "deltarune" {
		names = []string{"deltarune.win", "dr_textures.bin", "dr_music.bin"}
	}
	source := filepath.Join(common, names[0])
	names = append(names, ".butterscotch-"+o.Game+".json")
	check := func() error {
		if err := installDirectories(root); err != nil {
			return err
		}
		for _, name := range names {
			if err := installRegular(filepath.Join(common, name), true); err != nil {
				return err
			}
		}
		return nil
	}
	if err := check(); err != nil {
		return result, err
	}
	if err := installOriginals(common); err != nil {
		return result, err
	}
	stage, err := os.MkdirTemp(filepath.Dir(common), ".butterscotch-install-")
	if err != nil {
		return result, err
	}
	preserve := false
	defer func() {
		if preserve {
			return
		}
		if cleanupErr := cleanup(stage); cleanupErr != nil {
			message := fmt.Sprintf("cleanup could not remove temporary files at %s: %v", stage, cleanupErr)
			if err != nil {
				err = fmt.Errorf("%w; %s", err, message)
			} else {
				if result.Warning != "" {
					result.Warning += "; "
				}
				result.Warning += "Game files installed; " + message
			}
		}
	}()
	ready := filepath.Join(stage, "ready")
	progress := func(s string) {
		if o.Progress != nil {
			if s == "Preparation complete" {
				s = "Conversion complete"
			}
			o.Progress(s)
		}
	}
	var deletionPlan []installInput
	inspect := func(d *discovered) error {
		if !o.DiscardOriginals {
			return nil
		}
		var err error
		deletionPlan, err = installDeletionPlan(o.Game, common, d)
		return err
	}
	if _, err := prepare(Options{Game: o.Game, Source: source, Output: ready, Progress: progress}, inspect, common); err != nil {
		return result, err
	}
	prepared := filepath.Join(ready, "Assets/butterscotch/common")
	if err := os.Rename(filepath.Join(ready, "PREPARED-DATA.json"), filepath.Join(prepared, names[3])); err != nil {
		return result, err
	}
	expected := make(map[string]string, len(names))
	for _, name := range names {
		p := filepath.Join(prepared, name)
		if err := installRegular(p, false); err != nil {
			return result, err
		}
		digest, err := installFileHash(p)
		if err != nil {
			return result, err
		}
		expected[name] = digest
	}
	if err := check(); err != nil {
		return result, err
	}
	backup := filepath.Join(stage, "backup")
	if err := os.Mkdir(backup, 0700); err != nil {
		return result, err
	}
	type replacement struct {
		name                string
		backedUp, installed bool
	}
	changed := make([]replacement, 0, len(names))
	rollback := func(cause error) (InstallResult, error) {
		var failures []string
		for i := len(changed) - 1; i >= 0; i-- {
			item := changed[i]
			target := filepath.Join(common, item.name)
			if err := installDirectories(root); err != nil {
				failures = append(failures, err.Error())
				break
			}
			if err := installRegular(target, true); err != nil {
				failures = append(failures, err.Error())
				continue
			}
			if item.backedUp {
				if err := rename(filepath.Join(backup, item.name), target); err != nil {
					failures = append(failures, err.Error())
				}
			} else if item.installed {
				if err := os.Remove(target); err != nil {
					failures = append(failures, err.Error())
				}
			}
		}
		if len(failures) > 0 {
			preserve = true
			return result, fmt.Errorf("installation failed: %w; rollback incomplete (%s); recovery files preserved at %s", cause, strings.Join(failures, "; "), backup)
		}
		return result, fmt.Errorf("installation failed; previous files restored: %w", cause)
	}
	for _, name := range names {
		if err := check(); err != nil {
			return rollback(err)
		}
		target := filepath.Join(common, name)
		changed = append(changed, replacement{name: name})
		item := &changed[len(changed)-1]
		if _, err := os.Lstat(target); err == nil {
			if err := rename(target, filepath.Join(backup, name)); err != nil {
				return rollback(err)
			}
			item.backedUp = true
		} else if !os.IsNotExist(err) {
			return rollback(err)
		}
		if err := rename(filepath.Join(prepared, name), target); err != nil {
			return rollback(err)
		}
		item.installed = true
	}
	// Verify bytes after installation before releasing backups or any originals.
	for _, name := range names {
		if err := check(); err != nil {
			return rollback(err)
		}
		digest, err := installFileHash(filepath.Join(common, name))
		if err != nil {
			return rollback(err)
		}
		if digest != expected[name] {
			return rollback(fmt.Errorf("installed %s failed verification", name))
		}
	}
	if o.DiscardOriginals {
		result.OriginalsDiscarded = true
		for _, input := range deletionPlan {
			removeErr := check()
			if removeErr == nil {
				removeErr = installInputPath(common, input.path)
			}
			if removeErr == nil {
				removeErr = installRegular(input.path, false)
			}
			if removeErr == nil {
				current, hashErr := installFileHash(input.path)
				removeErr = hashErr
				if removeErr == nil && current != input.digest {
					removeErr = fmt.Errorf("original changed during conversion; retained")
				}
			}
			if removeErr == nil {
				removeErr = discard(input.path)
			}
			if removeErr != nil {
				result.OriginalsDiscarded = false
				if result.Warning != "" {
					result.Warning += "; "
				}
				result.Warning += fmt.Sprintf("Game files installed, but retained original %s: %v", input.path, removeErr)
			}
		}
	}
	if o.Progress != nil {
		o.Progress("Installation complete")
	}
	return result, nil
}

func installDirectories(root string) error {
	p := root
	for _, part := range []string{"", "Assets", "butterscotch", "common"} {
		if part != "" {
			p = filepath.Join(p, part)
		}
		info, err := os.Lstat(p)
		if err != nil {
			return fmt.Errorf("check card directory %s: %w", p, err)
		}
		if info.Mode()&os.ModeSymlink != 0 || !info.IsDir() {
			return fmt.Errorf("card directory must not be a symlink or non-directory: %s", p)
		}
	}
	return nil
}

func installRegular(p string, optional bool) error {
	info, err := os.Lstat(p)
	if optional && os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("game file must be a regular file, not a symlink or directory: %s", p)
	}
	return nil
}

func installOriginals(source string) error {
	return filepath.WalkDir(source, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.Type()&os.ModeSymlink != 0 || (!d.IsDir() && !d.Type().IsRegular()) {
			return fmt.Errorf("originals must contain only directories and regular files; unsafe path: %s", p)
		}
		return nil
	})
}

// installFileHash verifies packs without allocating another pack-sized buffer.
func installFileHash(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	digest := sha256.New()
	if _, err := io.Copy(digest, file); err != nil {
		return "", err
	}
	return hex.EncodeToString(digest.Sum(nil)), nil
}

type installInput struct{ path, digest string }

// installDeletionPlan claims only referenced raw audio; discover intentionally
// packs additional Undertale audio, which is not authorization to remove it.
func installDeletionPlan(game, common string, d *discovered) ([]installInput, error) {
	selected := bytes.ToLower(d.data)
	otherName := "deltarune.win"
	if game == "deltarune" {
		otherName = "data.win"
	}
	var other []byte
	protectAll := false
	if _, err := os.Lstat(filepath.Join(common, otherName)); err == nil {
		data, readErr := readInput(filepath.Join(common, otherName))
		if readErr != nil {
			protectAll = true
		} else if _, parseErr := Parse(data); parseErr != nil {
			protectAll = true
		} else {
			other = bytes.ToLower(data)
		}
	} else if !os.IsNotExist(err) {
		protectAll = true
	}
	var plan []installInput
	for name, path := range d.external {
		if !inside(path, common) {
			return nil, fmt.Errorf("music is outside common: %s", path)
		}
		lowerName := strings.ToLower(name)
		if !installOwnsAudio(selected, lowerName) {
			continue
		}
		stem := strings.TrimSuffix(lowerName, ".ogg")
		if protectAll || bytes.Contains(other, []byte(lowerName)) || (stem != "" && bytes.Contains(other, []byte(stem))) {
			continue
		}
		digest, err := installFileHash(path)
		if err != nil {
			return nil, err
		}
		plan = append(plan, installInput{path, digest})
	}
	if game == "deltarune" {
		plan = append(plan, installInput{filepath.Join(common, "audiogroup1.dat"), hash(d.groups[1])}, installInput{filepath.Join(common, "lang/lang_en.json"), hash(d.raw["lang_en.json"])})
	}
	return plan, nil
}

func installInputPath(common, path string) error {
	if !inside(path, common) || path == common {
		return fmt.Errorf("unsafe original path: %s", path)
	}
	for parent := filepath.Dir(path); parent != common; parent = filepath.Dir(parent) {
		info, err := os.Lstat(parent)
		if err != nil {
			return err
		}
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("original parent is not a regular directory: %s", parent)
		}
	}
	return nil
}

// installOwnsAudio requires a whole NUL-terminated filename or final path
// component. A suffix inside another track name is not an ownership claim.
// Uncertain dynamic references are deliberately retained.
func installOwnsAudio(lowerData []byte, lowerName string) bool {
	needle := append([]byte(lowerName), 0)
	for offset := 0; offset < len(lowerData); {
		match := bytes.Index(lowerData[offset:], needle)
		if match < 0 {
			return false
		}
		start := offset + match
		if start == 0 || lowerData[start-1] == 0 || lowerData[start-1] == '/' || lowerData[start-1] == '\\' {
			return true
		}
		offset = start + len(needle)
	}
	return false
}
