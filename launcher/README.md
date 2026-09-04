# zLauncher

The standalone entry point to zEngine. It manages a list of projects and opens
them in the editor; it does **not** link the engine and is built as its own
CMake solution.

## What it does

- **Tabs** — the nav strip has **Projects** and **Engine**. The Engine tab lists
  every GitHub release that ships an `engine_<n>.zip` (launcher releases are not
  shown) with its tag and build number; each row offers **Download**, or
  **Launch** + **Delete** once that build is downloaded. Deleting only removes a
  build from the per-user `%LOCALAPPDATA%\zEngine\Engine\Downloads\` folder.
- **Project list** — every registered project is shown with **Play**, **Edit**,
  **Build** and **Delete** buttons, plus a metadata line (version, created,
  launched, built — each shown only when known). The list lives in
  `%LOCALAPPDATA%\zLauncher\projects.txt`.
- **Bottom bar** — status message on the left, `zLauncher <v>  ·  Editor <v>` on
  the right. The editor version comes from a `version.txt` next to `zEngine.exe`,
  its version resource, or a nearby `CMakeCache.txt`.
- **+ New Project** — asks for a name, a location and a template, then:
  1. creates `<location>\<name>\` with a `<name>.zproject`, an `Assets` folder,
     and a `<name>.zlaunch` file (copying a template verbatim when one is chosen),
  2. registers it, and
  3. writes `%LOCALAPPDATA%\zEngine\editor.state` and launches `zEngine.exe`, so
     the editor starts up on the new project.
- **Play** — runs `<project>\builds\current\<exe>` (prefers `<name>.exe`).
  Greyed out when that folder has no executable.
- **Edit** — opens the project in the editor (same launch path as New Project).
- **Template** — copies the whole project into the editor's `templates\<name>\`
  folder (skipping `builds\` and `.git\`) so it can be chosen as a template in
  **+ New Project**. Fails if a template with that name already exists.
- **Delete** — removes a project from the launcher list. Never deletes files.
- **Update** — updates the launcher itself first (see below), then checks GitHub
  for a newer editor: downloads the latest release's `engine_<n>.zip` (or any
  `.zip` asset / the source zipball), unzips it with `tar`, and installs it as a
  new `C:\Program Files\z engine\versions\<n>\` folder alongside any existing
  versions, writing `version.txt`. The Start-menu and Desktop shortcuts are then
  repointed at the newest installed editor.
- **Editor install / shortcuts** — the launcher looks for the editor at
  `C:\Program Files\zEngine\zEngine.exe`, then the newest `<n>` folder under
  `%LOCALAPPDATA%\zEngine\Engine\Downloads\`. When nothing is installed it
  offers to download the latest `engine_<n>.zip` release; it installs to
  `C:\Program Files\zEngine\` when it can write there, otherwise it falls back
  to `%LOCALAPPDATA%\zEngine\Engine\Downloads\<n>\` (no administrator rights
  needed). Either way it (re)creates a `zEngine Editor.lnk` in the Start menu
  and on the Desktop pointing at the newest editor, and the outcome of an
  explicit download is always reported in a dialog.
- **`.zlaunch` files** — one per project folder (`ZLAUNCH 1`: name / version /
  icon / created / launched). On first run the launcher back-fills them into
  existing projects and registers a per-user file association so double-clicking
  a `.zlaunch` opens that project in the editor.

## Self-update

The launcher bakes in a build number (`ZLAUNCHER_BUILD_NUMBER`, set when cutting a
release) and a build date. **Update** checks the repo's releases for a
`launcher_<n>.zip` asset with `n` higher than the running build; if it finds one
it spawns `zLauncherUpdate.exe`, which waits for the launcher to close, downloads
and extracts the zip over the launcher folder, and relaunches it.

Cut a release by bumping `ZLAUNCHER_BUILD_NUMBER`, building Release, and
uploading `builds/release/launcher_<n>.zip` (the two exes) as a release asset.

## Finding the editor

The launcher looks for the override file first, then
`C:\Program Files\zEngine\zEngine.exe`, then the newest `<n>` folder under
`%LOCALAPPDATA%\zEngine\Engine\Downloads\`, then `zEngine.exe` next to itself,
then in `Release\` / `Debug\` subfolders, then one folder up, and finally walks
up looking for the engine's `builds\release\Release` / `builds\debug\Debug`
output (dev convenience). To point it somewhere explicit, create
`%LOCALAPPDATA%\zLauncher\editor_path.txt` containing the full path to
`zEngine.exe`. `%LOCALAPPDATA%\zLauncher\repo.txt` (`owner/repo`) overrides which
GitHub repository updates come from (default `CoolBeanGames/zEngine`).

## Build

From a Visual Studio Developer PowerShell at the repo root:

```powershell
cmake -S launcher -B launcher/builds/release -G "Visual Studio 18 2026" -A x64
cmake --build launcher/builds/release --config Release
.\launcher\builds\release\Release\zLauncher.exe
```

`cmake` is not on `PATH` in this environment; it ships with Visual Studio at
`…\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
`launcher/builds/` is git-ignored.
