# KCD2 Lua Development Tools
![vscodeheader](https://github.com/user-attachments/assets/43212ffd-a96b-45ac-aa6a-9206d0bd32c7)
This project provides tools for Lua development in Kingdom Come: Deliverance 2, consisting of:
- A VS Code extension for sending Lua code to the game
- A DLL mod (`.asi`) that enables convenient runtime Lua code execution in the game

It is loaded into the game with the [Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) and communicates with VSCode via a local TCP socket to maintain compatibility with the game running on Linux under Wine.

## Building

### Prerequisites

You'll need the following packages installed. On Arch based distros:

```bash
# Basic build tools
sudo pacman -S base-devel git cmake

# MinGW-w64 cross-compiler toolchain
sudo pacman -S mingw-w64-gcc

# Node.js and npm (for VS Code extension)
sudo pacman -S nodejs npm
```

For other Linux distributions, install equivalent packages using your package manager.

On Windows you will need CMake and a compatible Visual Studio version.
```ps1
winget install cmake
winget install --id=Microsoft.VisualStudio.2022.Community  -e
```

### Building the DLL Mod - Linux

Clone the repository with submodules:
```bash
git clone --recursive https://github.com/yobson1/kcd2lua.git
cd kcd2lua
```

Navigate to the cpp directory and run the build script:
```bash
cd cpp
chmod +x build.sh
./build.sh
```

### Building the DLL Mod - Windows

Clone the repository with submodules:
```bash
git clone --recursive https://github.com/yobson1/kcd2lua.git
cd kcd2lua
```

Generate a Visual Studio solution:
```ps1
cd cpp
cmake -B build -G "Visual Studio 17 2022" .
```
Open the .sln in Visual Studio and build

### Building the VS Code Extension

```bash
# Navigate to the vscode_extension directory
cd vscode_extension

# Install dependencies
npm install

# Build the extension
npm run compile
```

## Installation

### KCD2 Mod

- Install [Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) to the same directory as your KingdomCome.exe `KingdomComeDeliverance2/Bin/Win64MasterMasterSteamPGO/` I use the dinput8.dll version
- Copy the vscodelua.asi file to the same directory
- On Linux add this environment variable to your game's launch options `WINEDLLOVERRIDES="dinput8=n,b"`

### VS Code Extension

Install from the [VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=yobson.kcd2-lua-run)

## Usage

Usage instructions can be found in [the extension's readme](vscode_extension/README.md#getting-started)\
Note that the asi and extension's major version must match to be compatible. 1.X.X will work with 1.X.X but not 2.X.X+

## Debugging

- The DLL mod creates a `kcd2lua.log` file in the game directory
- If launched with `-console`, debug output will also appear in a separate console window
- The VS Code extension outputs connection status and Lua execution results in its output panel

## Acknowledgements
[Oren/ecaii](https://github.com/ecaii) - [Original code](https://github.com/ecaii/kcd2-lua-extension)
