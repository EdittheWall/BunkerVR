# Third-party licenses

BunkerVR's own code is under the [MIT License](../../LICENSE). It is built with the following, each under its own
license:

| Component | Used for | License | File |
|---|---|---|---|
| [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) (loader) | Talking to the headset's runtime | Apache 2.0 (the loader is Apache 2.0 or MIT) | [OpenXR-SDK.txt](OpenXR-SDK.txt) |
| [JsonCpp](https://github.com/open-source-parsers/jsoncpp) (inside the OpenXR loader) | Reading runtime manifests | MIT or public domain | [JsonCpp.txt](JsonCpp.txt) |
| [MinHook](https://github.com/TsudaKageyu/minhook) (with the Hacker Disassembler Engine) | Hooking the game's functions | BSD 2-Clause | [MinHook.txt](MinHook.txt) |
| [Dear ImGui](https://github.com/ocornut/imgui) | The developer's hand-pose tool on the PC screen | MIT | [Dear-ImGui.txt](Dear-ImGui.txt) |
| [DejaVu fonts](https://dejavu-fonts.github.io/) | Glyphs for some in-headset text | Bitstream Vera and Arev font licenses, DejaVu changes public domain | [DejaVu-Fonts.txt](DejaVu-Fonts.txt) |

Amnesia: The Bunker is the property of Frictional Games and is not covered by any of these licenses. This repository
contains none of its files.
