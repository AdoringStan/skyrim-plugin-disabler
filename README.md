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
[ ] add support for mod manager (MO2, Amethyst) with a simple .ini entry to give a path to another plugins.txt
[ ] add an ingame notification for the amount of plugins disabled
[ ] add an .ini option to automatically close and relaunch the game after plugins.txt is modified if preload isn't available