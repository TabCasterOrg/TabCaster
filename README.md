# TabCaster

A C implementation of TabCaster Server which acts as a screen manager and serves a virtual display to companion client app.

# Building and Running Tabcaster

## Prerequisites

Make sure you have the required development packages installed:

**Ubuntu/Debian:**
```bash
sudo apt install build-essential libx11-dev libxrandr-dev libxcvt-dev libxfixes-dev
```

**Fedora/RHEL:**
```bash
sudo dnf install gcc libX11-devel libXrandr-devel  libxfixes-delvel
```

**Arch:**
```bash
sudo pacman -S gcc libx11 libxrandr libxfixes
```

## Building

**Clone the project:**
```bash
git clone https://github.com/GuranshS/TabCaster.git
```

**Compile the project:**
```bash
make
```

**Clean build files:**
```bash
make clean
```

**Rebuild from scratch:**
```bash
make clean && make
```

The build will be placed into the `build/` folder.

## Running

**Basic usage:**
```bash
./build/tabcaster
```

**Example output:**
```
Tabcaster - C Version
Found 2 connected monitors
Connected screens:
  eDP-1: 1920x1080+0+0 (primary)
  HDMI-1: 2560x1440+1920+0
```

**Help Command:**
```bash
./build/tabcaster
```
**Example output:**
```
Usage: ./build/tabcaster [options]
Options:
  --list                    List all outputs and their status
  --create-mode WxH@R       Create CVT mode (e.g., 2336x1080@60)
  --add-mode OUTPUT ID      Add existing mode (by ID) to output
  --remove-mode OUTPUT ID   Remove mode (by ID) from output
  --delete-mode ID          Delete mode (by ID) from XRandR entirely
  --reduced-blanking        Use reduced blanking for CVT (with --create-mode)
  --help                    Show this help

Examples:
  ./build/tabcaster --create-mode 2336x1080@60
  ./build/tabcaster --add-mode HDMI1 123456789
  ./build/tabcaster --remove-mode HDMI1 2336x1080_60.00
```

## Troubleshooting

**"Cannot open X display" error:**
- Make sure you're running in a graphical session
- Check `echo $DISPLAY` shows something like `:0`
- Try `xrandr` command to verify X11 is working

**Compilation errors:**
- Install missing development packages (see Prerequisites)
- Check that `pkg-config --cflags --libs x11 xrandr` works

**Permission denied:**
- Make sure the binary is executable: `chmod +x tabcaster`

The program will automatically detect all connected monitors and display their configuration - no command line arguments needed.

## Delta-based streaming protocol (raw RGB)

The server now sends only changed regions using XDamage:

- Persistent bitmap: the client maintains a full-size RGB framebuffer.
- Deltas: when regions change, the server sends one or more rectangular RGB updates.
- Keyframes: periodic full-frame tiles are sent to re-sync.

### Packet format

```
struct PacketHeader {           // packed
  uint32_t frame_id;           // monotonically increasing
  uint16_t x, y;               // coordinates relative to capture origin (0..W/H)
  uint16_t w, h;               // rect size
  uint8_t  is_keyframe;        // 1 = keyframe, 0 = delta
  uint8_t  reserved[3];
  uint32_t data_size;          // bytes following, RGB24
};
// followed by data_size bytes of RGB24 for the rectangle
```

- Max UDP packet size is `MAX_PACKET_SIZE` (default 1400). Large rects are tiled vertically.
- Coordinates are absolute; clients should blit at `(x, y)` into their persistent buffer.

### Client expectations

- Allocate persistent RGB24 buffer of the reported display size.
- On receiving a packet:
  - If `is_keyframe == 1`: overwrite destination region.
  - Else: apply patch to the region.
- Occasionally the server sends keyframes (default every ~4s). Clients can request a keyframe by disconnect/reconnect if needed.

### Notes

- Cursor is composited server-side via XFixes if available.
- XDamage is used to minimize bandwidth and latency.
