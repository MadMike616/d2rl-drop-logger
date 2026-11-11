# Third-party notices

Native byte witnesses are adapted from RuffnecKk-D2RLoader-Suite, plugins/auto-pickup/src/auto_interact/native_contract.hpp, plugins/auto-pickup/src/native.hpp, and plugins/mapsense/src/native_automap_marker.cpp. Reference: https://github.com/RuffDood/RuffnecKk-D2RLoader-Suite

The chest flag and interaction accessor witnesses are adapted from MapSense's `native_automap_poi.cpp` and `native_object_interact_contract.hpp`.

The local context and client player accessor witnesses are adapted from MapSense's `native_automap_marker.cpp`.

The local game pointer location and its store instruction were cross-referenced with the [Pyre-Flies Floating Damage native contract](https://github.com/Pyre-Flies/D2R-Mods/blob/main/plugins/floating-damage/NATIVE-CONTRACT.md). Drop Logger uses its existing native lookup with that pointer.

The item-data pointer instruction bytes were cross-referenced with Pyre-Flies Gem Bag UI's `identification-witnesses.json`.

MIT License

Copyright (c) 2026 RuffnecKk and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
