# PCSX-Redux Agentic Ghidra scripts

This directory contains scripts that connect Ghidra with PCSX-Redux Agentic. To configure Ghidra from an open code browser, go to `Window -> Bundle Manager`, click the green `+` button on the top right, and select this directory from your filesystem.

In order to run the scripts, go to `Window -> Script Manager`, then locate the script you want to run using the filters. Right click the script in the list, and select `Run Script`. You may want to assign a key binding in the same contextual menu to simplify user experience.

## ReduxSymbols
This script connects to PCSX-Redux Agentic from Ghidra using the web server and uploads symbols from the open code browser. The list merges with the emulator's existing symbols. Enable the web server in PCSX-Redux Agentic. The script hardcodes its host and port; change them if your web server settings differ.

## export_to_redux.py
_[Typed Debugger tutorial video](https://youtu.be/iFiCo1l5oIM)_

Before running this script, make its `main_address` variable the address of your `main()` function.

This script will generate two files consumed by the Typed Debugger (accessed from the Debug submenu):

 - redux_data_types.txt: this contains a description of all the structs in Ghidra's Data Type Manager.
 - redux_funcs.txt: this contains a description of all the functions in Ghidra's Function Manager.

This allows mapping structs defined in Ghidra to runtime memory in PCSX-Redux Agentic, with the possibility of viewing field values in accordance with their described types, editing them, setting read and write breakpoints for the fields, logging read and write instructions accessing them, and toggling those instructions; it also allows setting breakpoints for described functions where arguments have their values given in accordance with their described types, as well as toggling entire functions.
