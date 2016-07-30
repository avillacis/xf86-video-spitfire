# xf86-video-spitfire
Xorg X11 video driver for the OAK Spitfire OTI-64111 video card.

This is an UMS (User Mode Setting) video driver for the OTI-64111 video chipset, manufactured by Oak Technology 
(https://en.wikipedia.org/wiki/Oak_Technology) around 1995-1996. This is a very, very old video card, with the following
characteristics:
* Up to 8 Mb of video memory
* Supports 4, 8, 16, 24, 32 bits per pixel with depth up to 24 bits. However, the BIOS cannot set 32bpp modes.
* Support for Windows 3.1/95 GDI acceleration, including lines, patterns, and bitblts. No compositing or 3D acceleration.
* Supposed to support DDC2 for reading EDID from monitor.
* Supposed to support image transfers between system and video memory through both mastering and "CPU assisted" modes.
* Supposed to have up to 4 hardware video overlays with arbitrary scaling.

Unfortunately, documentation for this video chipset is nearly nonexistent. The closest available documentation is for the 
OTI-64107 video card, its immediate prececessor. Therefore the implemented support does not fully exercise even the meager
capabilities of this video card.

This driver supports:
* basic modesetting exercised at 8, 16, 24 and 32 bpp
* continuous pixel clock setting up to 135 MHz
* minimal XAA/EXA acceleration for bitblt and pattern fill (XAA only, now deprecated in Xorg) operations within video memory.

Not supported due to lack of documentation:
* DDC/I2C support for EDID
* Hardware video overlays
* Hardware cursor
* I2C passthrough to slave devices such as video capture

BIG CAVEAT: in my particular setup (x86_64 with 2 Mb of video RAM in card), the 24 bpp almost always lock up the machine hard
after some video activity (about 1 minute). I am not sure whether the cause is some subtle bug in the video setup, or a result
of the card's age, or even the card being improperly seated in the PCI slot. Therefore, 32 bpp is highly recommended. Also, 
because the acceleration engine has severe limitations on 24 bpp and is frequently restricted to grayscale only.

DESPERATELY SEEKING ACCURATE DATASHEET OR DOCUMENTATION!!! If you can point me to an accurate datasheet for the 64111 (NOT the
64107), please contact me.
