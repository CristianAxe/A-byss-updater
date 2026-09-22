# Abyss Updater (name on the works)

**Abyss Updater** is a lightweight and clean Atmosphère and (some) homebrew updater.

---

## Features

* **Atmosphere CFW Management:** Check the 5 previous releases and update directly from the Switch, version tracking seems to be working (needs further testing).
* **Hekate Updater:** Downloads the latest release from Hekate + Nyx.
* **Homebrew Apps:** I've added the ones that I use the most, will add more in a later date.
* **Live-ish Installation Status:** Trackes whether a Homebrew app is  `[Installed]` or `[Not Installed]`, needs further testing.

---

## Supported Homebrew & Patches

* **EdiZon** (Overlay & App)
* **Patches (`fs-patch`)** (Sigpatches for running backups/homebrew)
* **Sphaira**
* **JKSV** (Save data manager)
* **Checkpoint** (Save manager)
* **CaptureSight**

---

## Controls

| Button | Action |
| :--- | :--- |
| **D-Pad Up / Down** | Navigate through menu items |
| **A Button** | Select category / Install selected item |
| **B Button** | Go back / Return to previous menu |
| **Plus Button (+)** | Exit the application |

---

## Installation

1. Go to the [Releases](https://github.com/CristianAxe/A-byss-updater/releases) page of this repository.
2. Download the latest `.nro` file.
3. Place the file onto your Nintendo Switch SD card inside the `switch` folder (`sdmc:/switch/abyss-updater/abyss-updater.nro` or similar).
4. Launch the **Homebrew Menu** on your Switch and open the Updater.

---

## Built With

* [libnx](https://github.com/switchbrew/libnx) - Nintendo Switch homebrew development library
* [SDL2](https://www.libsdl.org/) & [SDL2_ttf](https://www.libsdl.org/projects/SDL_ttf/) - Graphics and font rendering
* [libcurl](https://curl.se/libcurl/) - Network requests and GitHub API communication
* [libarchive](https://www.libarchive.org/) - Zip extraction
* [nlohmann/json](https://github.com/nlohmann/json) - JSON parsing

---

## Special Thanks To
* [Atmosphere](https://github.com/Atmosphere-NX/Atmosphere) - The entire Atmosphere development team
* [CTCaer](https://github.com/CTCaer/hekate/) - For Hekate and Nyx
* [CostelaBR](https://github.com/CostelaCNX/CNX) - CostelaBR, whose CNX pack served as a semi-inspiration for me to try to develop something myself
* The entire switch modding and development community
