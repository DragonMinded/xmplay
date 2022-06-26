xmplay
======

An incredibly simple music player for Sega Naomi. Set up your toolchain and environment at https://github.com/DragonMinded/libnaomi and then add any number of music files to a `romfs/` folder and compile with make. Then you can load this into Demul or onto actual hardware with a net dimm and listen! Select with up/down on the 1P/2P joystick and play the selected song with "Start". This was originally put together as a simple test of the full libnaomi suite, including audio, threads, input and 3rd party library linking.

The following formats are supported:

 - mod
 - xm
 - it
 - s3m
 - midi (with gravis ultrasound soundfont)
 - mp3
 - ogg
