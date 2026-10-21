# ProOS

ProOS is a self-hosted home automation and home server platform by Ben Provenzano III. It holds the configuration, firmware and code for every machine on the home network, from a Proxmox server down to a Pico W lamp controller, and one web app, **Automate**, controls all of it from a phone or browser.

The system has three layers. At the centre is one Proxmox server, which routes the network and runs storage, media, backups and the Automate web app in containers and VMs. Around the apartment, Raspberry Pis each take one role, such as a LED display, an audio receiver, a camera or a serial bridge. Below them, Pico W and Arduino microcontrollers switch the lamps, amps, outlets and bed. A button press in Automate goes to its API on the server, which calls the Pi or microcontroller that owns the device, so the browser never talks to the hardware directly. Every machine is defined entirely by its folder in this repo and is rebuilt over the network from a stock OS install, so any host can be replaced from scratch.

### Features

- **Automate web app.** One page that drives the apartment room by room: lamps and dimmers, ceiling lights, TVs, PCs (wake-on-LAN, sleep, power relays), the stereo amps' power, inputs, volume and subwoofers, the motorised bed and its LED strip, and the bathroom's motion-sensing light. Also live room status, scenes (room, lights, all on / off), temperature and humidity, weather, network host status, bookmarks, encrypted shared lists, photo and video upload, and a live doorway camera.
- **LED pixels.** Pis drive LED walls, grids and strips through Fadecandy or WS2812 directly, with C++ effects (text scroll, spectrum, maze, noise, orbits and more), video playback on the pixels, shuffle, and per-host brightness, kept in sync from one control.
- **Raspberry Pi modules.** A shared Pi base plus one small module per Pi: AirPlay and Bluetooth audio receivers, LED hosts, IR and motion-recording cameras with cloud upload, and the bedroom hub's serial bridge. Each Pi runs a read-only root filesystem so it survives losing power, recovers its own Wi-Fi, falls back to a setup hotspot, and compiles its C/C++ programs on install.
- **Microcontroller firmware.** Pico W lamp and light controllers with their own HTTP API, and Arduino/AVR boards for the bedroom hub, bed motor, switched outlets, the integrated amp and an IR remote hub.
- **Home server.** A Proxmox host running the services as containers and VMs: the router, SMB file shares (including one for vintage clients), an AFP server for classic Macs, Jellyfin with automatic MP3 mirroring, a UniFi controller, and a sandboxed desktop with a VPN kill switch. ZFS mirrored storage with automatic snapshots, rsync backups to encrypted external pools, encrypted on-demand storage regions, UPS monitoring, and backups of every VM and container, all triggered from the website.
- **One-command deployment.** `login.sh` deploys any host from this repo over SSH, with one passphrase prompt per terminal session for all keys.

The Automate website (`shared/html`, `shared/nodeapi`) has its own full reference, [`shared/CLAUDE.md`](shared/CLAUDE.md), imported at the end of this file.

## The system at a glance

One Proxmox server (`pve`) runs the network's services as LXC containers and KVM VMs. Raspberry Pis around the apartment each run one "module" (LEDs, cameras, audio), and small microcontrollers (Pico W, Arduino) drive lamps, amps and the bed. The Automate website, served from the `automate` container, is the remote control for all of it.

```
                    browser / phone
                          │  https://automate.home
                          ▼
  ┌─────────────────────────── pve (Proxmox, 192.168.1.8) ──────────────────────────┐
  │  automate LXC 106 ── lighttpd → node API ──┬─ HTTP → Pis (port 80 /api)          │
  │      /var/www/html/ram  ═══ bind ═══╗      ├─ HTTP → Pico W lamps (port 4000)    │
  │                                     ║      └─ LED hosts (mainmenu.txt)           │
  │  host tmpfs /mnt/ramdisk ◄══════════╝                                            │
  │      ▲ file-monitor.sh → apicmds.sh (backups, regions, start/stop VMs)           │
  │  files LXC 101 (SMB) · mgmt LXC 102 · legacy LXC 104 · media LXC 108             │
  │  router VM 100 · appletalk VM 103 · xana VM 105 · unifi VM 107                   │
  │  ZFS: rpool (boot) · tank/datastore (data, mirrored) · scratch SSD · ext. backups │
  └──────────────────────────────────────────────────────────────────────────────────┘
       Pis: brpi · ledwall · ledgrid · ledstrip · doorway · homepi · btpi · lrpi
       Microcontrollers: BathroomLight, LivingRoomLamp (Pico W) · Bedroom hub, amp, IR hub (AVR)
```

## Folder map

Every top-level folder is one of four kinds. `login.sh` tells them apart by the marker file in the folder: `lxc.conf`, `qemu.conf` or `pc.conf` means a server ("nonpi"), anything else is a Pi module.

| Kind | Folders | Layout | Deployed by |
| --- | --- | --- | --- |
| Pi base | `rpi/` | Programs, `config/` (installer, systemd units, system configs), `effects/` (LED programs) | Copied to every Pi first, to `/opt/rpi` |
| Pi module | `brpi` `btpi` `doorway` `homepi` `ledgrid` `ledstrip` `ledwall` `lrpi` | `boot.sh`, `modconf/` (`brand.txt`, `modinit`, `config.txt`, other configs), extra programs or firmware | Overlaid onto `/opt/rpi` after the base |
| Server (LXC / VM / PC) | `automate` `files` `legacy` `media` `mgmt` (LXC); `appletalk` `xana` (VM); `pve` (the Proxmox host); `brpc` (a physical PC) | `config/installer.sh` plus the files it installs; `lxc.conf` / `qemu.conf` / `pc.conf` marker | Tarred to `/tmp/config` and `installer.sh` run |
| Not deployed | `shared/` (Automate site, deployed with `automate` and every Pi), `wkst/` (this workstation, `install.sh` run by hand) | | |

`login.sh` refuses `rpi`, `wkst`, `shared`, `sources`, `login`, `logon` and `.ssh` as host names.

## Network and hosts

DNS domain `home`, LAN 192.168.1.0/24, gateway and DNS the router at 192.168.1.1. Addresses known from the code:

| Host | Kind | Address | What it is |
| --- | --- | --- | --- |
| `router` | VM 100 | 192.168.1.1 | The router, with two NICs passed through over PCIe (`hostpci0/1`); login is `admin@router`, not root. Configs in `pve/network/` (pfSense, EdgeRouter, UniFi backups) |
| `files` | LXC 101 | .4 | Samba file server (SMB2+): shares Data, Media, Scratch, Cameras, Games, MP3s, Regions, External |
| `mgmt` | LXC 102 | .7 | Management box: holds the SSH keys (`mgmt/keys/`, git-ignored) and runs `login.sh`; mounts the Projects share read-only at `/mnt/Projects` |
| `pve` | Proxmox host | .8 | Hypervisor; 4 NICs bonded 802.3ad into `vmbr0` |
| `appletalk` | VM 103 | | Netatalk 4 AFP server (built from `netatalk-4.4.3.tar.xz`) for vintage Macs |
| `legacy` | LXC 104 | .19 | Samba with `LANMAN1` minimum protocol, for old clients; started on demand from the website |
| `xana` | VM 105 | | Debian desktop over TigerVNC: Firefox in Firejail, Transmission behind a PIA WireGuard tunnel with kill switch and watchdog, retro themes |
| `automate` | LXC 106 | .16 | The Automate website and node API |
| `unifi` | VM 107 | | UniFi network controller |
| `media` | LXC 108 | .11 | Jellyfin, plus `mp3sync` (C++, `mp3sync.cpp`): watches the music library with inotify and mirrors it as MP3s into the MP3s share |
| `brpc` | PC | .17 | Bedroom Lenovo ThinkCentre: its own lighttpd + node API (`server-min.js`) and `webapi.sh` so Automate can sleep it; woken by wake-on-LAN |
| `brpi` | Pi | .15 | Bedroom Pi (wired, Wi-Fi off): AirPlay, serial bridge to the bedroom hub and the amp, relax sounds |
| `ledwall` | Pi | .12 (`lrxmit` in the API) | LEDwall: the 93-pixel LED ring (Fadecandy) and the living room's command host. Its `custom.js` serves `extcom` (living room stereo codes over serial on `/dev/ttyAMA0`), the white LED, the front button and the desktop PC power relay (`pc_power`) |
| `ledgrid` | Pi Zero | | WS2812 grid driven directly (`opcserver`) |
| `ledstrip` | Pi | | 3×60 strips (Fadecandy) |
| `doorway` | Pi | | IMX462 IR camera: `rtspcam` (RTSP on :8554/cam), `minimotion` recorder, `ldrdetect` day/night, rclone to Google Drive, Cameras share |
| `homepi` | Pi | | Family-home camera hub: `motion` with two cameras, uploads via rclone |
| `btpi` · `lrpi` | Pi | | Audio receivers: AirPlay (shairport-sync) and Bluetooth (BlueALSA) into a stereo; `btpi` adds Bluetooth to a vintage stereo, `lrpi` (formerly `stereo`) is the living room's, branded "Living Room" |
| `desk` | PC | .14 | Living room desktop, pinged by `wkststate` |
| `lrmlamp1` · `bthrlight1` | Pico W | .18 · .21 | Living room lamp, bathroom light (firmware in `automate/devices/`) |
| `wkst` | Workstation | | This machine: mounts the `files` shares under `/mnt/*` with autofs (`auto.files`), detached while `files` is down by `share-watchdog.sh` (so this repo, `/mnt/Data/Projects/ProOS`, is the Data share) |

The Automate status menu (`shared/html/menus/statsmenu.txt`) and the LED host list (`menus/mainmenu.txt`) are the living lists of hosts the website talks to.

## Deployment: `login.sh`

`login.sh` runs on `mgmt`, where the repo is at `/mnt/Projects/ProOS` (its `ROOTDIR`), and needs `/opt/deploy` as a scratch folder. A host's key (`mgmt/keys/<host>.rsa` for servers, `rpi.rsa` for all Pis) has a passphrase when the host's folder holds an empty `ssh.pwd` marker file (`<host>/ssh.pwd`; for the Pi key, `rpi/ssh.pwd` or any Pi module's); all marked keys share one passphrase. Marked keys go through one `ssh-agent` per terminal session (keyed by the calling shell's PID, socket under `$XDG_RUNTIME_DIR/ssh-agents/`, or `/dev/shm/proos-<uid>/` without it): the first marked login in a terminal adds every marked key in one `ssh-add` call, so a single `Enter passphrase:` prompt unlocks them all (the script is its own `SSH_ASKPASS` prompter, so no key path is shown; Enter skips a key for the rest of the session), and later runs reuse the agent. Unmarked keys are used straight from the file. Each connection uses `-o IdentitiesOnly=yes -i` with the host's key (its public key taken from the agent when unlocked), so ssh never offers the other loaded keys. The agent is removed by `login clearkeys`, by the `EXIT` trap in `mgmt/config/profile_ben`, when its shell is gone, or after 8 h; the passphrase itself is never stored. It checks port 22 with `nc`, then:

| Command | Does |
| --- | --- |
| `login <host>` | SSH shell as root (`admin` for `router`) |
| `login <host> sync` | Deploy: server → `DEPLOY_SERVER`, Pi → `DEPLOY_PI` |
| `login <pi> reset` | Deploy a Pi after deleting `/etc/rpi-conf.done`, so the installer's first-run phase runs again |
| `login <module> init <host>` | Deploy module `<module>` onto the Pi currently reachable as `<host>` (renames it to `<module>`) |
| `login <pi> <cmd>` | Runs `rpi <cmd>` on the Pi (see `rpi/rpi.sh`) |
| `login <host1>,<host2>,… [<cmd>]` | Opens one tmux tile per host (a new window when already inside tmux), each running `login <host> [<cmd>]` with the same `<cmd>` (not `init`). The keys are unlocked once before the tiles open and the tiles share that terminal's agent (`PROOS_AGENT_DIR`); each tile waits for a key before it closes. `mgmt` installs `/etc/tmux.conf` with `mouse on`, so a click selects a tile |
| `login update-server` | `apt upgrade` on every folder with an `lxc.conf` / `qemu.conf` |
| `login clearkeys` | Forget this terminal's unlocked keys (stops its agent) |
| `login cmds` · `login rmtmp` | Command cheat sheet · delete deploy temp files and stop every session agent |

**A new Pi.** Start from stock Raspberry Pi OS with root SSH login and password authentication enabled, then run `login <module> init <host>`: the first connection falls back to the root password, and the installer replaces root's `authorized_keys` with `rpi/config/authorized_keys` (the public half of `mgmt/keys/rpi.rsa`, git-ignored) and installs an `sshd_config` that allows root by key only (`PermitRootLogin without-password`, `PasswordAuthentication no`).

After a deploy a menu offers re-deploy, update packages, reboot (a Pi reboots into read-only mode with `rpi ro`) or a shell.

**Server deploy.** Copies `<host>/config` to a temp folder (for `automate`, also `shared/html` and `shared/nodeapi` into it), tars it, `scp`s it to `/tmp`, untars to `/tmp/config` and runs `/tmp/config/installer.sh` as root. Every server installer follows the same shape: apt packages, users (`tank` uid 1000 and group `shared` gid 1000, which the LXC idmaps pass through unshifted), sshd config and `authorized_keys`, `profile`, `sysctl.conf`, `rc.local` + `rc-local.service` as the boot hook, service files, then deletes `/tmp/config`.

**Pi deploy.** If the root filesystem is an overlay it first runs `rpi rw` and waits for the reboot. Then it rsyncs, in order: `rpi/` → `/opt/rpi`, `shared/html/` → `/opt/rpi/config/html/`, `shared/nodeapi/` → `/opt/nodeapi/`, and `<module>/` → `/opt/rpi/` (excluding `photos` and `sources`), writes the module name to `/opt/rpi/config/hostname` and runs `/opt/rpi/config/installer.sh`.

Because the module is copied over the base, **a module's files replace the base's files of the same name**: `<module>/boot.sh` replaces `rpi/boot.sh`, and `modconf/` lands at `/opt/rpi/modconf/`.

## Raspberry Pi platform (`rpi/`)

Requires Raspberry Pi OS on Debian **trixie**; the installer refuses anything else.

### Installer (`rpi/config/installer.sh`)

1. **First run only** (no `/etc/rpi-conf.done`): locale, time zone, `pi` user, a long apt package list (build tools, GStreamer, ffmpeg, Bluetooth, node, lighttpd, motion; no X11 desktop), purges cron, logrotate, swap, PulseAudio/PipeWire, cups and others, deletes old `rpi-*` units.
2. **Every run**: read-only boot partition, `config.txt`, sshd and key, `sysctl`, NetworkManager, journald (to RAM), systemd units from `config/systemd/` into `/etc/systemd/system/`, `/etc/netmode.sh` and `/etc/preinit.sh`.
3. **Node API**: moves `rpi/controller.js`, `rpi/ledparser.js` and `rpi/custom.js` over the shared ones in `/opt/nodeapi` (`npm install` only when `package.json` changed, tracked in `.deps.hash`).
4. **Website**: rsyncs the staged html into `/var/www/html` (keeping `ram/`), replaces the title `>Automate<` with the module's `brand.txt` name (this is how `main.js` picks the Pi or LED Pi page model), minifies, and gives `www-data` sudo on `servercmds.sh` and `relaxrun.sh` only.
5. Bluetooth, ALSA, Samba USB share, udev USB automount, motion defaults, hostname and `/etc/hosts`.
6. **`config/cleanup.sh`**, once per Pi (skipped while `/etc/rpi-cleanup.done` exists; delete it and deploy to run it again), so the system fits an 8 GB card: swaps `libopencv-dev` for `libopencv-core-dev` + `libopencv-videoio-dev`; purges the X11 desktop, the `arduino` apt package and its AVR toolchain, USB Wi-Fi dongle firmware, the `v6` kernels, every kernel but the running and newest, and all kernel headers; deletes `arduino-cli` and `~/.arduino15` unless the module's `modinit` installs it (only `brpi` does); installs `config/dpkg-nodoc.conf` as `/etc/dpkg/dpkg.cfg.d/01-nodoc` and deletes docs, man pages and non-English translations; clears the apt lists and npm cache.
7. **`config/compile.sh`**: builds the C/C++ programs (below).
8. First run: enables `ssh avahi-daemon proinit NetworkManager timesyncd`, disables the rest (lighttpd, bluetooth, smbd, hostapd …), touches `/etc/rpi-conf.done`.
9. Runs `/opt/rpi/modconf/modinit` (the module's own installer), then **deletes `/opt/rpi/modconf`**; restarts lighttpd and nodeapi; links `/usr/bin/rpi` → `rpi.sh`; deletes `/opt/rpi/config`.

### Boot sequence

`proinit.service` (after `network-online.target`) runs `/etc/netmode.sh boot`, then `/opt/rpi/boot.sh` — the module's own boot script. Module boot scripts start only the services that module needs, passing settings through `systemctl set-environment` (e.g. `rpi_ledtype`, `rpi_ledmaxbright`, `rpi_gpipin_1`, `rpi_imx*`), which the `rpi-*.service` units read. Nothing module-specific is enabled at boot; services are started by `boot.sh`.

### Read-only root

Pis run with an OverlayFS root, so writes go to RAM and the Pi can be unplugged safely (the SD cards used to corrupt when a Pi lost power). `rpi rw` / `rpi ro` reboot into read-write / read-only (via `raspi-config nonint disable_overlayfs`/`enable_overlayfs`); the boot partition is remounted rw only for the moment it is written. Anything that must survive a reboot on a Pi has to be written in rw mode or to the boot partition / a USB drive.

### Networking (`rpi/config/netmode.sh`)

`boot`, `client`, `apd`, `delwifi`, `watchdog`. Wi-Fi credentials live on the boot partition (`/boot/firmware/wpa.conf`, `NAME|$|PASSWORD`, written by the website's Wi-Fi setup through `servercmds.sh confwpa`). Flags on the boot partition: `disable.wifi`, `apd.enable` (start as hotspot `RPiHotspot`). With no known network at boot the Pi falls back to hotspot mode with a captive DNS (`captive.conf`). `rpi-netwatchdog.service` (`netmode.sh watchdog`) recovers a hung Wi-Fi chip in stages without writing to disk; tunables are at the top of the script.

### `rpi` command (`rpi/rpi.sh`)

`ro`, `rw`, `update`, `expand`, `net`, `wifiscan`, `wifistats`, `togglenet`, `client`, `apd`, `apdonboot`, `enablewifi`/`disablewifi`, `delwifi`, `resetbt`, `restart-alsa`, `temp`, `jl`/`jh` (journal), `running`/`active`/`timers`/`loadtimes`. The website's Pi options call these through `servercmds.sh`, which logs to `/var/www/html/ram/sysout.txt`.

### Pi-side node API

The Pis run the same `shared/nodeapi/server.js`, but `rpi/controller.js` replaces the server's command dispatch: `server <cmd>` → `sudo servercmds.sh <cmd>`, `confwpa` → Wi-Fi setup, everything else → `exeCustom()` in `custom.js`. The base `rpi/custom.js` is a stub; a module with real commands ships `modconf/custom.js`, which `modinit` copies over it:

- `brpi/modconf/custom.js`: serial via `ztermcom` (one FIFO queue, at most 8 waiting) to the amp on `/dev/ttyUSB0` and the bedroom hub on `/dev/ttyAMA0` (`hwcom`, `usbcom`), amp power/inputs/volume, `bed_leds_hex_set`, relax sounds (`relaxmenu.txt`, `relaxrun.sh` → `rpi-relaxloop.service`), activity LED flash.
- `ledwall/modconf/custom.js`: `extcom` (serial), `front_btn` (LED shuffle), white LED, `pc_power`.

`rpi/ledparser.js` replaces the server's LED fan-out with the local effect runner: it spawns one effect binary from `/opt/rpi/effects`, plays videos through `mp4toleds`, and sets brightness through the proxy. Shuffle picks from `menus/ledshuffle.txt`. Each command also records what the pixels show, which `fc_status` (`/api?action=leds&arg=fc_status`) answers as JSON with the brightness: `{ effect, state, color, brightness }`, state `off` (stopped, or black), `active`, `paused` (also an effect that ended on its own) or `color` (RRGGBB); the server's Pixels window reads it.

### Local services and ports

| Program (source) | Unit | Port / interface | Role |
| --- | --- | --- | --- |
| `gposerver` (`gposerver.cpp`) | `rpi-gposerver` | HTTP 127.0.0.1:8770 `/gpio?pin=&state=HIGH\|LOW\|TOGGLE` | GPIO outputs for the node API |
| `pinmon` | `rpi-pinmon-1` | GPIO input | On a falling edge runs `/opt/rpi/<cmd>.sh` (ledwall: `front_btn.sh`, which calls the local API) |
| `ldrdetect` | `rpi-ldrdetect` | GPIO | Light sensor day/night for the camera |
| `opcserver` (`effects/`) | `rpi-opcserver` | OPC :7890, HTTP :7892 | Drives WS2812 directly (ws281x DMA library); args `<WxHgrid> <power limit> <rotation>` |
| `fcserver` (prebuilt) + `fcproxy` | `rpi-fcserver`, `rpi-fcproxy` | effects → :7890 proxy → :7891 fcserver; HTTP :7892 brightness | Fadecandy hosts; layouts in `effects/fc.<ledtype>.json` |
| `mp4toleds` | `rpi-mp4toleds` | OPC client, HTTP :7860 `/api?video=` | Plays `effects/videos/*.mp4` on the pixels |
| Effects: `textscroll maze oscope spectro noise orbits miami stripes chase writecolor` | spawned by `ledparser.js` | OPC client → :7890 | One running at a time; share `ledlayout.cpp` and `opcclient.cpp` |
| `rtspcam` | `rpi-rtspcam` | RTSP :8554/cam | IMX462 hardware H.264 (doorway; notes in `rtspcam.md`) |
| `minimotion` | `rpi-minimotion` | | Motion recorder: reads the RTSP stream, records in `/mnt/ramdisk`, copies clips out (`doorway/minimotion.conf`) |
| `ztermcom` (`ztermcom.c`) | CLI | serial | Framed serial request/response to Arduino hubs |
| node API · lighttpd | `nodeapi`, `lighttpd` | :3000 · :80 | Same as the Automate server |

`ledtype.txt` (written by a LED module's `modinit`) names the layout: `93ringinv` (ledwall), `3x60strips` (ledstrip), `16x16grid` (ledgrid).

### Compiling (`rpi/config/compile.sh`)

C/C++ sources are compiled on the Pi at install time. `build()` compiles only when the source's sha256 differs from its archived copy in `/opt/source-backup`, the binary is missing, or a shared dependency changed (`opcutils.*` / `httpapiserver.*` for the servers, `ledlayout.*` / `opcclient.*` for the effects); the source is then **moved** into `/opt/source-backup`. A new program needs a `build`/`effect`/`tool` line here. The ws281x library is built once into `/usr/local`.

## Microcontroller firmware

| Folder | Board | Talks to | Role |
| --- | --- | --- | --- |
| `automate/devices/BathroomLight/` | Pico W | HTTP :4000 | Bathroom light + C4002 mmWave sensor; API reference `.md` alongside |
| `automate/devices/LivingRoomLamp/` | Pico W | HTTP :4000 | Living room lamp (switched lamp + dimmable LED, rotary encoder); derived from BathroomLight |
| `automate/devices/InfraredRemoteHub/` | Arduino | serial | Sends IR codes to the stereo's preamp, DAC and sub amp |
| `brpi/BedroomHub/` | AVR | brpi `/dev/ttyAMA0` | Bedroom hub #1: relays, temperature/humidity (DHT), two external serial ports |
| `brpi/BedroomHubExt/` | ATmega328P | hub serial #2 | WS2812 bed strip, switched outlets, bed motor (EAGLE board files alongside) |
| `brpi/BedroomPowerExt/` | AVR | serial | 2×120 VAC + 12 VDC switched controller |
| `brpi/IntegratedAmp/` | Arduino Uno | brpi `/dev/ttyUSB0` | Integrated amp controller; flashed from the Pi with `brpi/update-amp.sh` (`update-bedhub.sh` for the hub) via `arduino-cli` |

Pico firmware is built with the arduino-pico core (`build/rp2040.rp2040.rpipicow/`, git-ignored); Wi-Fi credentials are in `creds.h` (git-ignored). Pico commands use the `http://<host>:4000/api?<cmd>,,<value>~` form, errors as text under HTTP 200.

## Proxmox host (`pve/`)

`pve/config/installer.sh` sets up the host: no-subscription apt source, NIC name pinning (`50-pve-nic*.link`) and the bonded `interfaces`, IOMMU / PCIe passthrough (`grub`, `blacklist.conf`), swap moved to the scratch SSD, `fstab`, postfix mail relay (`sasl_passwd`, git-ignored), apcupsd (APC SC620 UPS), smartd, lm-sensors, ZFS auto-snapshots on `tank/datastore` (hourly/daily/weekly/monthly cron, no 15-minute), scrub/trim cron, `sys-check` and `kernelclean` tools, and the services below. `pve/qemu.conf` is only a placeholder so `login.sh` treats `pve` as a server. `pve/README.txt` holds the ZFS runbooks (new mirror, encrypted backup drive, replacing a data or boot drive) and `pve/zfsman` is a ZFS helper.

Storage: `rpool` (boot, ZFS), `tank` mirrored with `tank/datastore` at `/mnt/datastore` (`data`, `media`, `.regions`), a scratch SSD at `/mnt/pve/scratch` (camera footage, downloads, MP3s, uploads, swap), encrypted external backup pools listed in `bkpdrives.txt` attached under `/mnt/extbkps`.

### Website → host commands

This is how the Automate Server window controls the host:

1. The host mounts a 128 MB tmpfs at `/mnt/ramdisk` (owner `tank:shared`, setgid), bind-mounted into the automate LXC as `/var/www/html/ram`.
2. The node API's `server <name>` command appends to `ram/sysout.txt` and creates an empty `ram/<name>.txt`.
3. `file-monitor.service` (`pve/config/file-monitor.sh`) watches the ramdisk with inotify, claims each `*.txt` trigger atomically into `spool/`, and runs `/usr/bin/apicmds.sh <name> /mnt/ramdisk` in the background (at most 4 at once), appending output to `sysout.txt`. Data files are on its `IGNORE` list (`sysout.txt`, `*.save.txt`, `*.lock`, `pwd.txt`, `fusearch.txt`, `drives.txt`, `*.tmp`) — a new data file kept in `ram/` must be added there or it will be consumed as a command.
4. The website polls `sysout.txt` into the log window.

`apicmds.sh` commands: `backupstd`, `backupchk` (rsync to external pools, lock `backupsvr.lock`), `last_backup_dates`, `attach_bkps`, `detach_bkps`, region mounts (`mnt_/unmnt_arch_region` gocryptfs Archive, `mnt_/unmnt_ram_region`, `mnt_/unmnt_snap_region`, `unmnt_all`), `pve_listsnaps`, `start/stop` for `legacy` (LXC 104), `xana` (105, plus `restorexana`), `unifi` (107), `atalk` (103), `rebootrouter`, `syslog` (`sys-check`), `pve_vmsbkp`, `clearlog`. A new server command needs a case here and an option in `shared/html/server.js`.

### Persistent state and backups

- **Website state** (`bookmarks.save.txt`, `lists.save.txt`, their lock and password files) lives only in the RAM disk. `hourly.sh` (`hourlyjob.timer`, and `hourlyjob-sd.service` on shutdown) copies them to `/mnt/datastore/data/Documents/` (`Bookmarks.txt`, `Lists.txt`, keeping a `.bak`), and restores them into the RAM disk when they are missing after a reboot.
- **`pve_vmsbkp`** vzdumps every guest, copies the archives into `pve/vmbkps/`, and **overwrites `<host>/lxc.conf`, `appletalk/qemu.conf`, `xana/qemu.conf` and `pve/vmbkps/vzdump-qemu-*.conf` in this repo** from `/etc/pve`. Those files are generated snapshots of the live config: edit the guest in Proxmox, not here.

## Conventions across the repo

Code style rules (naming, comments, shell, JavaScript and CSS) live only in [`CODESTYLE.md`](CODESTYLE.md), imported at the end of this file. This section covers how the repo is organised and worked on.

- A new Pi service is a `rpi-<name>.service` in `rpi/config/systemd/`, not enabled, started from the module's `boot.sh` with settings passed by `systemctl set-environment`.
- A new Pi module is a folder with `boot.sh` and `modconf/{brand.txt,modinit,config.txt}`; `brand.txt` becomes the page title, AirPlay/Bluetooth/Samba name.
- A new server is a folder with `config/installer.sh` and a marker file; add its key as `mgmt/keys/<name>.rsa`, plus an empty `<name>/ssh.pwd` if the key has the shared passphrase.
- Secrets are git-ignored and must stay out of commits and docs: `mgmt/keys/`, `*.rsa`, `*.pub`, `*.pem`, `*.crt`, `*.key`, `authorized_keys`, `auto.creds`, `creds.h`, `sasl_passwd`, `pia-wg.conf`. Don't print their contents.
- `._*` and `.DS_Store` files are macOS metadata, ignored by git; leave them alone.
- Large binaries in the tree (`pve/vmbkps/*.zst`, `rpi/effects/videos/`, firmware builds, photos) are backups or assets, not sources to read. Photos of the builds are in `btpi/photos`, `ledstrip/photos`, `ledwall/photos` and `pve/photos`, and are not deployed.
- There is no git or node in this workstation's shell, and no test suite: verify shell changes by reading them, and the website as described in the Automate reference.

Code style: [CODESTYLE.md](CODESTYLE.md)

@CODESTYLE.md

## Automate website reference

Automate reference: [shared/CLAUDE.md](shared/CLAUDE.md)

@shared/CLAUDE.md
