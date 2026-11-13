# DevoQ3Map2
DevoQ3Map2 is a map compiler for id Tech 3 games by **Devoted**. It's a fork of the [id Tech 3 Map Compiler](https://github.com/id-tech-3-tools/map-compiler), which is a modified version of [Q3Map2](http://q3map2.robotrenegade.com/).

DevoQ3Map2 tries to bring more shaders and global illumination into the game. It's aimed at Jedi Academy (`-game ja`) and the PBR renderer used by [DevoJK](https://github.com/Dadunito/DevoJK), and is meant to stay compatible with the other games the compiler already supports.

> **Status:** early, work in progress. New shader and lighting features are listed in the [changelog](changelog.md) as they're added.

## Goals
* **More shaders:** support more shader keywords and surface options at compile time, so maps can use richer materials.
* **Global illumination:** better bounced light (radiosity), ambient light and lightgrid results, so lighting looks more natural in modern renderers.
* **Keep what works:** existing maps, switches and compile scripts should keep working.

## DevoGI: baking DevoJK's lighting ahead of time
DevoJK's renderer gives maps new, high resolution lighting (baked global illumination with soft shadows) the first time a map loads, and saves it as `maps/<name>.devogi`. That bake runs on the CPU and can take a few minutes for a big map at high settings.

The `-devogi` stage runs exactly the same bake (the same `devogi_core.cpp` file as the renderer) from the command line, without starting the game. It reads a finished BSP, does not change it, and writes the `.devogi` file. The BSP can be a file on disk or a map inside the game's pk3 files:

```
mapcompiler -game ja -fs_basepath "C:/Games/EternalJK" -fs_game EternalJK -devogi -scale 8 maps/mp/ffa3.bsp
```

Put the result in the game's `maps` folder (for example `EternalJK/maps/mp/ffa3.devogi`, or ship it in a pk3 with the map). The game uses it when the player's `r_devoGI_*` settings match the options used for the bake; otherwise it bakes its own. The options have the same names and defaults as the game's settings: `-scale`, `-rays`, `-bounces`, `-aa`, `-sheen`, `-intensity`, `-exposure`, `-sky`, `-saturation`, `-ao`, `-aodistance`, `-aodirect`, `-glow` (`r_devoLights_glow`), `-relight`, `-entitylights`, plus `-threads` and `-out <file>`. Run `mapcompiler -help devogi` for the full list. Use the same `-fs_game` as the players, so the bake sees the same shaders and textures they do.

## Credits
DevoQ3Map2 builds on many years of work by others. Thank you to:
* **id Software**, for the Quake III Arena engine and the original q3map tools released under the GPL.
* **Randy "ydnar" Reddig** and the GtkRadiant contributors, for creating Q3Map2 and its lighting system.
* **The NetRadiant / Xonotic developers**, for maintaining [NetRadiant's q3map2 2.5.17n](https://gitlab.com/xonotic/netradiant/tree/225d4bba22a409bc1c375a2029d2162d336f0070), which the id Tech 3 Map Compiler is forked from.
* **isRyven, Aciz and the [id-tech-3-tools](https://github.com/id-tech-3-tools) team**, for the id Tech 3 Map Compiler: the new VFS, lightgrid and floodlight fixes, model conversion, the cleaned-up switches and much more (see the [changelog](changelog.md)).
* The authors of the bundled libraries listed below.

## Documentation
* [Changelog](changelog.md): all API changes, bug fixes and new features, including those from the id Tech 3 Map Compiler.
* [Upstream wiki](https://github.com/id-tech-3-tools/map-compiler/wiki): detailed information on every switch the compiler supports.
* [scripts](scripts): batch scripts for compiling, decompiling and model conversion.
* [Upstream releases](https://github.com/id-tech-3-tools/map-compiler/releases/): precompiled Windows binaries of the original id Tech 3 Map Compiler.

## Author
* **Devoted**

## Contributing
Feature requests, bug reports and pull requests are welcome under issues. See [CONTRIBUTING.md](CONTRIBUTING.md) for more information.

Fixes that also apply to the original compiler are worth sending to [id-tech-3-tools/map-compiler](https://github.com/id-tech-3-tools/map-compiler) as well.

## External dependencies
All required dependencies are shipped with the project.  
[assetsys](https://github.com/mattiasgustavsson/libs/blob/master/docs/assetsys.md) loads binary assets into virtual file system.  
The library used in a project contains [custom changes](https://github.com/isRyven/libs). MIT License.  
[Catch2](https://github.com/catchorg/Catch2) test framework for unit-tests, TDD and BDD. Boost Software License.  
[filesystem](https://github.com/wjakob/filesystem) a simple class for manipulating paths on Linux/Windows/Mac OS. BSD-3 License.  
[jpeg-compressor](https://github.com/richgel999/jpeg-compressor) jpeg compression/decompression lib. Public domain.  
[lodepng](https://github.com/lvandeve/lodepng) PNG encoder and decoder. zlib License.  
[msdirent.h](https://svn.apache.org/repos/asf/avro/trunk/lang/c/tests/msdirent.h) dirent API for MS Windows. MIT License.  
[tinyformat](https://github.com/c42f/tinyformat) type safe printf replacement library. Boost Software License.

## Compiling binaries

### Compiling on Linux
```
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=RELEASE .. && make
```

### Compiling on Windows
Several options are available.

#### Visual Studio
Recent Visual Studio versions can open the CMake project folder directly: `File > Open > Folder` (or `File > Open > CMake` in VS2017), then build from the CMake menu.

#### Compile from command line
```
mkdir build
cd build
cmake ..
cmake --build . --target mapcompiler --config Release
```

#### Generate Visual Studio solution
```
mkdir build
cd build
cmake ..
```
Open the generated `sln` in Visual Studio and build the solution from there. Pass `-G "<generator>"` to pick a specific Visual Studio version.

# Original Q3Map2 License
```
Copyright (C) 1999-2007 id Software, Inc. and contributors.
For a list of contributors, see the accompanying CONTRIBUTORS file.

This file is part of GtkRadiant.

GtkRadiant is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

GtkRadiant is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GtkRadiant; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

----------------------------------------------------------------------------------

This code has been altered significantly from its original form, to support
several games based on the Quake III Arena engine, in the form of "Q3Map2."
```
