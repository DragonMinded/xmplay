xmplay
======

An incredibly simple XM (and other format) player for Sega Naomi. Set up your toolchain and environment at https://github.com/DragonMinded/netboot/tree/trunk/homebrew and then add any number of tracker files to the `romfs/` folder and compile with make. Then you can load this into Demul or onto actual hardware with a net dimm and listen! Select with up/down on the 1P/2P joystick and play the selected song with "Start".

TODOs
=====

 - Ability to enter/leave directories to choose files within them.
 - Use realpath() once it is in the libnaomi stdlib.
 - Hold up/down and get scrolling without tapping a billion times.
