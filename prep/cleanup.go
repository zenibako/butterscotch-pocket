package prep

import (
	"os"
	"path/filepath"
)

// removeTree uses the basic filesystem operations available in TinyGo WASI,
// where os.RemoveAll is unimplemented. Symlinks are removed as leaves.
func removeTree(path string) error {
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if info.IsDir() {
		entries, err := os.ReadDir(path)
		if err != nil {
			return err
		}
		for _, entry := range entries {
			if err := removeTree(filepath.Join(path, entry.Name())); err != nil {
				return err
			}
		}
	}
	err = os.Remove(path)
	if os.IsNotExist(err) {
		return nil
	}
	return err
}
