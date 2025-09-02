= Testing

A brief description of how the program is tested, and some basic knowledge

== X11 Errors

There are a few notable X11 errors that have been run into:

| Phase              | XError    | Major Opcode | Minor Opcode | Method | OS |
|--------------------|-----------|-----|----|--------------------|-------|
| Mode Creation      | BadName   | 140 | 16 | RRCreateMode       | Arch
| Adding Output      | BadMatch  | 140 | 18 | RRAddOutputMode    | Arch
| Enabling Output    | BadMatch  | 140 | 21 | RRSetCrtcConfig    | Arch
| Deletion Of Output | BadMatch  | 140 | 19 | RRDeleteOutputMode | Mint
| Deletion Of Mode   | BadAccess | 140 | 17 | RRDestroyMode      | Mint

Some of these errors are only on Arch, some are on Mint.

Do note that the Arch error might be due to the NVIDIA card in my machine.

Here is the general output of an XError:

```
X Error of failed request:  BadMatch (invalid parameter attributes)
  Major opcode of failed request:  140 (RANDR)
  Minor opcode of failed request:  19 (RRDeleteOutputMode)
  Serial number of failed request:  17
  Current serial number in output stream:  18
```

The program always halts afterwards.

Full list of X11 error handlers: https://tronche.com/gui/x/xlib/event-handling/protocol-errors/default-handlers.html

Make sure to check that all outputs of TabCaster are closed. If you need a quick way to test, reset X11 and sign back in.

== Testing

Testing is primarily done on Cinnamon Desktop, with Linux Mint. Additional testing is done with i3 and Cinnamon on Arch.
