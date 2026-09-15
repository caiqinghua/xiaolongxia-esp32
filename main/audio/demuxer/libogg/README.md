# libogg framing subset

Unmodified Xiph.Org libogg 1.3.6 files: `src/framing.c`, `src/crctable.h`,
`include/ogg/ogg.h`, `include/ogg/os_types.h`, and `COPYING`.

Source: https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz

Archive SHA-256:
`5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061`

`include/ogg/config_types.h` is the Audream C99 fixed-width type configuration
for ESP-IDF and Linux. macOS uses the original `os_types.h` platform branch.
Only framing is compiled; the bit-packing API is not used or linked.
The BSD-style license in `COPYING` must accompany distribution.

This library is private to the watch recording reader. The original prompt
demuxer and other boards are unchanged. The wrapper imposes packet and per-call
work limits before submitting pages to the stream reassembler.
