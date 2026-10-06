# Plugin Disabler
Plugin Disabler is an SKSE plugin that automatically disables Skyrim plugins (esl, esp, and esm files) on game load. This may require a restart depending on how fast your game loads, and is less likely on newer versions of Skyrim. If no plugins are disabled, then nothing happens and your game launches as usual.

!!!Back up your load order before use!!!

## y tho?
I have a few use cases for this actually:
- merged patches (for example, those included in many of the QND Spid Packs) could include a file to automatically disable the plugins they merge
- certain xedit and Synthesis patchers can convert plugins to BOS/Skypatcher/etc files. If a plugin is 100% replaced, the patcher could generate a config for this plugin to automatically disable those plugins to save space in the LO
- mod list creators - some mods require other mods, but ask you to disable their esps. If a modlist creator wants to keep users from accidentally enabling esp files, they can include a configuration for this.

# configuration

## disabling plugins
Create one or more `.json` files in `Data/SKSE/plugins/PluginDisabler` in the
Skyrim game installation. Each file must contain a JSON array of plugin names:

```json
[
	"plugina.esp",
	"pluginb.esl",
	"pluginc.esm"
]
```

## ini options

### mod manager profile path

The plugin always updates the standard Local AppData `plugins.txt` path. To
also update one or more mod manager profile files, the ini file can be edited to add up to 64 paths:

```ini
[Paths]
PluginsTxtPath1=Z:\path\to\first\profile\plugins.txt
PluginsTxtPath2=Z:\path\to\second\profile\plugins.txt
```

I use Amethyst mod manager since I am on Linux, so I will not be adding special MO2 or Vortex support. Adding a path to your plugins.txt should suffice for most mod managers. Please let me know if this doesn't work with your setup. Use the path for the profile the manager launches with.

Under Linux/Proton, enter a Windows-style path visible to the game; `Z:` usually maps to the Linux
filesystem.

### auto-closing the game

After Skyrim finishes loading its plugins, Plugin Disabler checks whether any
plugin it changed is still loaded in the current session. If none are loaded,
it reports that verification succeeded. If any remain loaded, the message box
offers to quit Skyrim without listing each plugin; the names of still-loaded
plugins are written to `PluginDisabler.log`. Set `ForceTerminateIfPluginsStillPresent=1` to make any
verification-failure message OK-only and quit the game when acknowledged,
regardless of SKSE version. The default is `1`, which will autoclose the game.

```ini
[General]
ForceTerminateIfPluginsStillPresent=1
```

On SKSE 2.2.7 and newer, the plugin still edits `plugins.txt` during preload,
before Skyrim loads its plugins, and then verifies the result after the
data-loaded message. The setting changes only what happens if verification
finds one or more targeted plugins still loaded.

## requirements
- SKSE
- Address Library

## caveats
- Smaller load orders are more likely to need to restart after launch because the game loads faster than the plugin can work. This is less likely on newer Skyrim/SKSE versions.
- Creation Club content acts weird when disabled. This is a native Skyrim thing, not a Plugin Disabler thing. Avoid disabling CC content (and vanilla content) with this.
- The popup window confirming a plugin disabler run may minimize the Skyrim window in some instances.
- While I own SkyrimVR, I do not have a headset, and since the game won't open for me without one, I can't test it. I will need to work with VR users to get this working. I'd like to do this because I can especially see this being helpful for VR users since they can't use esl plugins.

# how it works
The plugin combines the names from all JSON files in SKSE/Plugins/PluginDisabler, ignoring filename case and duplicate entries. It removes the enabled `*` marker from matching lines in
`%LOCALAPPDATA%/Skyrim Special Edition/plugins.txt` (or `Skyrim VR/plugins.txt`
for Skyrim VR) as well as any other configured plugins.txt files. It does not remove the entries from the file. SKSE 2.2.7 and newer run this change during preload, before Skyrim reads the
plugin list. After the game reports that its data is loaded, the plugin checks
the changed names against Skyrim's loaded regular and light-plugin lists. The
version check is retained: SKSE 2.2.7 and newer use preload; older versions use
the normal load callback as a fallback, followed by the same runtime check.

The post-load message only offers to quit when a plugin that was changed is
actually present in the current session. The INI option controls whether
acknowledging that message force-terminates the game or presents a Yes/No choice,
regardless of SKSE version.

Before editing each `plugins.txt`, the plugin reads enabled plugins' TES4
headers and checks their `MAST` dependencies. Requested dependents and masters
can be disabled together in the same edit. A requested master is left enabled
if any dependent will remain enabled, and the log names the dependent that
blocked it. If an enabled plugin header cannot be read, no requested entries
are changed in that `plugins.txt`, because its dependencies cannot be verified.

Invalid JSON files and invalid plugin names are skipped and reported in the
SKSE log.

# On AI Use

I used AI to help build this. It is tested and confirmed to be working on my end, and have also read through and reviewed all of it personally. My personal policy on AI usage is to not just use it as a crutch to build everything for me, but also as a tool to learn from, so I study all code generated by the AI to learn what it's doing and how it works to the best of my ability. That being said, while I do have some profesional experience with coding, it was mostly JS for websites and C# on the side for fun, so I am by no means an expert. I am a hobbyist. Please do not use this plugin if that makes you uncomfortable.

# For Developers

This has completely open permissions. If you would like to improve on this or want to steal the idea to completely redo it from the ground up for better performance and/or functionality, then please feel free to do so. The only request that I have is that you would keep it backwards compatible, but of course with open source even that is optional. 🙂

## Building

This is based on my [linux hellow world template](https://github.com/AdoringStan/commonlibsse-ng-template-hello-world-linux). You will first need to follow the one-time setup guide on the [Linux cross-compile readme](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/examples/linux-cross-compile/README.md) in [alandtse/CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG). After that, you should be able to run the build command in the project root:

```sh
cmake --preset build-release-linux-clangcl-vcpkg-all
cmake --build --preset release-linux-clangcl-vcpkg-all
```

I'm not sure on how to build for Windows since I do not have a Windows system, but the cmake config was borrowed from CommonLibSSE-NG, so it should have built in Windows support.

## Intellisense
For IntelliSense, install clangd in VS Code.

## TODO
- [X] test preload support on latest Skyrim version
- [X] add an ingame notification for the amount of plugins disabled
- [X] add an .ini option to automatically close and relaunch the game after plugins.txt is modified if preload isn't available
- [ ] add automated unit testing for plugin
- [x] check if a plugin will be disabled if it depends on a master, not just if it already has been
- [ ] insure that the game is still able to load plugins who's masters are being checked in ReadPluginMasters() since it's opening a file stream for each plugin while the game loads
- [ ] attempt to replace the custom esp header reading functions with [Ortham/esplugin](https://github.com/ortham/esk)
- [ ] instead of procesing each plugins.txt individually, just process the main game's plugins.txt and replace the others with it
- [ ] consider moving some of the code into another file
- [ ] clean up ini for release
- [ ] ~~test on SkyrimVR~~ I can't do this on my own - I need someone with VR to help with this.
- [ ] ~~check if extra plugins.txt paths in ini actually need to be numbered or not~~ it looks like the way ini files are read in cpp this wouldn't work.
