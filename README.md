Full disclosure: I smashed this template together with an unholy fusion of Bing, then Claude when I ran out of tokens, and then finally copilot in VSCode when I discovered it could read through all my code and make suggestions. I'm not aware of templates for building plugins on Linux, so I thought I'd do it myself. If anyone is aware of resources from people who actually know what they're doing, please let me know.

I used resources from the Linux cross-compiling in [alandtse/CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/examples/linux-cross-compile/README.md), [mrowrpurr's Logging SKSE Template](https://github.com/SkyrimScripting/SKSE_Template_Logging), and this [commonlibsse-ng-template](https://github.com/libxse/commonlibsse-ng-template). It's somehow working. Please God forgive me.

## Building
You will first need to follow the one-time setup guide on the [Linux cross-compile readme](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/examples/linux-cross-compile/README.md) in [alandtse/CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG). After that, you should be able to run the build command in the project root:

```sh
cmake --preset build-release-linux-clangcl-vcpkg-all
cmake --build --preset release-linux-clangcl-vcpkg-all
```

## Intellisense
For IntelliSense, install clangd in VS Code.

## Plugin Disabler configuration

Create one or more `.json` files in `Data/SKSE/plugins/PluginDisabler` in the
Skyrim game installation. Each file must contain a JSON array of plugin names:

```json
[
	"plugina.esp",
	"pluginb.esl",
	"pluginc.esm"
]
```

The plugin combines the names from all JSON files, ignoring filename case and
duplicate entries. It removes the enabled `*` marker from matching lines in
`%LOCALAPPDATA%/Skyrim Special Edition/plugins.txt` (or `Skyrim VR/plugins.txt`
for Skyrim VR). It does not remove the entries from the file.
SKSE 2.2.7 and newer run this change during preload, before Skyrim reads the
plugin list. Older SKSE versions use the normal load callback as a fallback;
if Skyrim has already read `plugins.txt` by then, the change takes effect on the
next launch instead.

### Mod manager profile path

The plugin always updates the standard Local AppData `plugins.txt` path. To
also update one or more mod manager profile files, create
`Data/SKSE/plugins/PluginDisabler.ini` with numbered absolute paths:

```ini
[Paths]
PluginsTxtPath1=Z:\path\to\first\profile\plugins.txt
PluginsTxtPath2=Z:\path\to\second\profile\plugins.txt
```

Use the path for the profile the manager launches with. Under Proton, enter a
Windows-style path visible to the game; `Z:` usually maps to the Linux
filesystem. Add entries sequentially as `PluginsTxtPath1`, `PluginsTxtPath2`,
and so on, up to `PluginsTxtPath64`; missing numbers are ignored. Invalid
relative paths are skipped. If no numbered path is set, only the default Local
AppData file is updated. The plugin does not discover or select manager profiles
automatically. Duplicate paths are processed once.

When one or more entries are disabled in the standard Local AppData
`plugins.txt`, a message box reports that file's count and the path to
`PluginDisabler.log`. Manager-profile files are still updated, but their counts
are not included in the dialog. With SKSE 2.2.7 or newer, it is informational
because preload runs before Skyrim reads the plugin list. With older SKSE, the
message offers to force-terminate Skyrim. The plugin does not relaunch the game. Start it again through the same
mod manager or launcher.
After a successful file update, `PluginDisabler.log` lists the disabled plugin
names under a section labeled with that `plugins.txt` path.

For SKSE versions older than 2.2.7, the default message asks whether to
force-terminate Skyrim. To instead make the message an OK-only notice that
force-terminates the game when acknowledged, add this to
`Data/SKSE/plugins/PluginDisabler.ini`:

```ini
[General]
ForceTerminateAfterFallback=1
```

This setting has no effect when preload is supported.

Invalid JSON files and invalid plugin names are skipped and reported in the
SKSE log. With a mod manager, verify that the resolved `plugins.txt` belongs to
the active profile before relying on the change.

## Testing
This is AI Generated, haven't personally test this:
Tests are disabled by default. To enable them, add the `tests` feature to
`default-features` in `vcpkg.json`:

```json
"default-features": [
	"tests"
]
```

Then change `BUILD_TESTS` in `CMakeLists.txt` from `OFF` to `ON` and re-run the
configure and build commands above.

## TODO
[ ] test preload support on latest Skyrim version
[X] add an ingame notification for the amount of plugins disabled
[X] add an .ini option to automatically close and relaunch the game after plugins.txt is modified if preload isn't available