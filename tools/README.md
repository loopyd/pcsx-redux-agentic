# PCSX-Redux Agentic tools

This directory contains tools designed to help with Linux PSX reverse engineering and behavioral analysis. The top level, directly usable tools are:

* [exe2elf](exe2elf) - Converts a PS-EXE executable to an ELF file, which can be useful for loading and debugging through gdb.
* [exe2exe](exe2exe) - Converts a PlayStation executable to a PS-EXE executable, normalizing the binary in the process.
* [exe2iso](exe2iso) - Converts a PS-EXE executable to a bootable ISO file containing a single PSX.EXE, conformant to the ISO9660 standard and bootable on a retail PlayStation 1.
* [ghidra_scripts](ghidra_scripts) - A collection of Ghidra scripts that can be used to integrate some parts of PCSX-Redux into Ghidra and vice versa.
* [ps1-packer](ps1-packer) - A tool for compressing PlayStation 1 executables into a single self-decompressing binary in various formats.
* [psyq-obj-parser](psyq-obj-parser) - A tool for parsing the object files produced by the Psy-Q SDK, and converting them to ELF files.

For the tools that need to be built, the top level Makefile can be used to build them all using the `tools` target.

The [linux-mips](linux-mips) folder contains scripts for generating a cross-compiler for the MIPS architecture.

The remaining folders contain internal tools and native Linux build support.

`../scripts/manage-psx-emulator-skill.sh` installs, checks, or removes the fork-owned skill links in an integrating project. It does not change home hooks or install dependencies.
