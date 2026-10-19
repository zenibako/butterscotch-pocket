package plugin

import "fmt"

func (s *Session) guidePlatforms() ([]byte, error) {
	return s.menu("guide-os", "Which computer has your game installed?", "macOS", "Windows", "Linux", "Back", "Exit")
}

func (s *Session) guidePage() ([]byte, error) {
	game := "UNDERTALE"
	if s.game == "deltarune" {
		game = "DELTARUNE"
	}
	steps := []string{
		fmt.Sprintf("1/3: Find the installation\nSteam: Library → %s → Manage → Browse local files.\nOther stores: open the installed game folder.", game),
		"",
		fmt.Sprintf("3/3: Copy resources to your SD card\nAssets/butterscotch/common/%s/\nKeep original filenames and subfolders.", s.game),
	}
	if s.game == "undertale" {
		switch s.guideOS {
		case "macOS":
			steps[1] = "2/3: Open the resources\nRight-click UNDERTALE.app → Show Package Contents → Contents → Resources.\nCopy this folder's contents."
		case "Windows":
			steps[1] = "2/3: Select the resources\nFind data.win and the original .ogg music.\nCopy this folder's contents, including any music subfolder."
		case "Linux":
			steps[1] = "2/3: Select the resources\nFind game.unx (or data.win) and the original .ogg music.\nCopy this folder's contents, including any music subfolder."
		}
	} else {
		chapter := map[string]string{"macOS": "chapter1_mac", "Windows": "chapter1_windows", "Linux": "chapter1_linux"}[s.guideOS]
		steps[1] = fmt.Sprintf("2/3: Select Chapter 1 resources\nFind %s and the shared mus folder.\nKeep all files inside the chapter folder.", chapter)
		if s.guideOS == "macOS" {
			steps[1] = "2/3: Select Chapter 1 resources\nRight-click DELTARUNE.app → Show Package Contents → Contents → Resources.\nSelect chapter1_mac and mus."
		}
		if s.guideOS == "Linux" {
			steps[1] = "2/3: Select Chapter 1 resources\nFind chapter1_windows (Steam/Proton) or chapter1_linux, plus mus.\nKeep the chapter folder intact."
		}
		steps[2] = "3/3: Copy the Chapter 1 folder and mus into:\nAssets/butterscotch/common/deltarune/\nKeep the two folders side by side."
	}
	next := "Next"
	if s.guideStep == len(steps)-1 {
		next = "Ready to install"
	}
	return s.menu("guide", steps[s.guideStep], next, "Back", "Exit")
}
