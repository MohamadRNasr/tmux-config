# Tmux Configuration

A personal customized Tmux setup with a minimal status bar, improved shell prompt, custom colors, and useful keyboard shortcuts.

**Note:** This configuration currently supports Linux only and depends on systemd, NetworkManager, and BlueZ.

## Preview

![Tmux Preview](assets/tmux-preview.png)

## Setup

### Automatic Installation

```bash
git clone https://github.com/mohamadrnasr/tmux-config.git ~/.config/tmux
cd ~/.config/tmux
./install
```

### Manual Installation

Clone the repository into the Tmux configuration directory:

```bash
git clone https://github.com/mohamadrnasr/tmux-config.git ~/.config/tmux
```

### Build Modules

If you installed the configuration manually, build the Wi-Fi and Bluetooth status modules:

```bash
cd ~/.config/tmux/modules/

# Build Wi-Fi status module
gcc -O2 -Wall -Wextra tmux_wifi.c -o tmux_wifi $(pkg-config --cflags --libs libsystemd)

# Build Bluetooth status module
gcc -O2 -Wall -Wextra tmux_bluetooth.c -o tmux_bluetooth $(pkg-config --cflags --libs libsystemd)
```

Close and reopen Tmux to apply the configuration.

## Dependencies

The following tools and services are required:

- **Nerd Font** – For displaying icons and special characters.
- **LazyGit** – For Git integration inside Tmux.
- **libsystemd** – For D-Bus communication.
- **systemd** – Required for system D-Bus.
- **NetworkManager** – For Wi-Fi status monitoring.
- **BlueZ** – For Bluetooth status monitoring.
- **GCC & pkg-config** – Required to build the status modules.

### Clipboard Utility

Depends on your desktop environment:

- `wl-copy` – Wayland (default configuration).
- `xclip` or `xsel` – X11.

If you are using a different clipboard utility, update the following line in `tmux.conf`:

```bash
bind-key -T copy-mode M-w send -X copy-pipe-and-cancel "wl-copy"
```

Replace `wl-copy` with the appropriate command:

| Environment | Command |
|---|---|
| Wayland | `wl-copy` |
| X11 | `xclip -selection clipboard` |
| X11 | `xsel --clipboard --input` |

## Features

- Custom color scheme and pane borders.
- Status bar moved to the top.
- Custom shell prompt with username, current directory, and time.
- SSH and Root session indicators:
  - Active windows use background colors to indicate SSH or Root sessions.
  - Inactive windows use foreground text colors for the same indicators.
  - Normal sessions have their own distinct color scheme.
- Wi-Fi status monitoring with signal strength and connectivity indicators.
- Bluetooth status monitoring with connected device count.
- Event-driven status monitoring without periodic polling.
- LazyGit integration through a popup (`Ctrl + g`).
- Terminal popup in the current pane's directory (`Ctrl + p`).
- Clipboard integration (`Alt + W` in copy mode).
- 24-hour clock mode (Ctrl + b t).
