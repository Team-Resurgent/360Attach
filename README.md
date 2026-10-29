# 360Attach

Mounts a CCI disc image as the Xbox 360's DVD drive and launches the game on it.
Built with Visual Studio 2010 and the Xbox 360 XDK (21256). Retail and RGH
consoles load the disc as it is. On a devkit, the disc's `default.xex` is
resigned in memory so the kit accepts it.

## Using it

Put the two built files in the folder that holds the image:

```
Hdd:\Games\MyGame\
    default.xex                       <- bin\Release\default.xex
    mount.xex                      <- bin\Release\mount.xex
    game.cci                       <- or game.iso; any name works
```

Launch `default.xex` from your dashboard. It mounts the first `.cci` in its
folder, then starts `\Device\Cdrom0\default.xex`. A split image
(`name.1.cci`, `name.2.cci`, ...) is picked up as one disc. Progress and errors
go to `Launcher.log` in the same folder and to the debug output.

Both trimmed images (game partition only) and full XGD2/XGD3 dumps work; for a
full dump the security sectors are read so XGD2 drive-info queries get real
values.

## How it works

- **Launcher** (`default.xex`, a title) finds the image, checks it with libCCI
  (headers, index, and an XDVDFS game partition), loads `mount.xex` as a
  system DLL from a system thread so it stays resident, calls its mount export,
  and launches the disc.
- **Mount** (`mount.xex`, a system DLL) replaces the `DRIVER_OBJECT` behind
  `\Device\Cdrom0` with its own:
  - reads are queued to a system worker thread that decodes sectors straight
    from the `.cci` (`CciStream.cpp`, reading only the index entries each
    request needs rather than holding the whole index in memory);
  - the DVD/SATA IOCTLs the kernel and XAM send while probing a disc are
    answered from the mounted image (`DvdHook.cpp`);
  - a fake tray open/close is broadcast to SMC listeners so the system drops
    the previous disc and probes the new one (`VTray.cpp`);
  - on a devkit (`XeKeysGetConsoleType`), the root `default.xex` is resigned
    in memory before the game launches (`XexDevkit.cpp`). The image key is
    rewrapped and the header is signed with the debug key, the same transform
    as XexTool `-m d`. Only those header sectors are substituted when the
    loader reads the file; the executable stays the same size, and the rest of
    it is still read from the image. A manufacturing executable, or one that
    is already debug-signed, is left unchanged. If the resign fails, the mount
    fails instead of launching a retail-signed executable.
  With nothing mounted, all requests pass through to the real drive driver.

The exports are listed in `common/CciMountApi.h`. `mount.xex` stays loaded
until reboot; if you replace it with a newer build, reboot before launching
again (the launcher refuses a resident copy with a different version).

## Building

```
git clone --recursive <this repo>
```

Open `360Attach.sln` in Visual Studio 2010 and build `Release|Xbox 360`, or:

```
MSBuild.exe 360Attach.sln /p:Configuration=Release "/p:Platform=Xbox 360"
```

Output goes to `bin\<Configuration>\`.

## Third-party code

- `third_party/libCCI` — [libCCI](https://github.com/Team-Resurgent/libCCI)
  (GPL-2.0-or-later) as a submodule, with its bundled LZ4 (BSD-2-Clause). The
  launcher links the reader; the driver uses its format constants and LZ4.
- `common/xdk` — the parts of xkelib (kernel and XAM headers, and the XDK 21256 `.def` files) that the launcher and driver use. The import libraries are generated from the `.def` files at build time.

## Status

Builds cleanly; not yet tested on hardware. Xenia cannot run it: its kernel
does not dispatch I/O to driver objects that the guest replaces.

## Reporting a problem

If the disc doesn't load, send:

1. `Launcher.log` from the game's folder. It records every step: the image it
   found, the mount result, and the status codes of any failed read or load.
2. The console's error message (for example "Unrecognized Disc"), or a photo of it.
3. If you can run the kit's debug output, the `[Launcher]` and `[CciMount]`
   lines from the time of the launch.

Status codes are hexadecimal NTSTATUS values, such as `C0000221` (image
checksum mismatch).
