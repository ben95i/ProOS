# ProOS

ProOS is a self-hosted home automation and home server platform by Ben Provenzano III. It holds the configuration, firmware and code for every machine on the home network, from a Proxmox server down to a Pico W lamp controller, and one web app, **Automate**, controls all of it from a phone or browser.

The system has three layers. At the centre is one Proxmox server, which routes the network and runs storage, media, backups and the Automate web app in containers and VMs. Around the apartment, Raspberry Pis each take one role, such as a LED display, an audio receiver, a camera or a serial bridge. Below them, Pico W and Arduino microcontrollers switch the lamps, amps, outlets and bed. A button press in Automate goes to its API on the server, which calls the Pi or microcontroller that owns the device, so the browser never talks to the hardware directly. Every machine is defined entirely by its folder in this repo and is rebuilt over the network from a stock OS install, so any host can be replaced from scratch.

### Features

- **Automate web app.** One page that drives the apartment room by room: lamps and dimmers, ceiling lights, TVs, PCs (wake-on-LAN, sleep, power relays), the stereo amps' power, inputs, volume and subwoofers, the motorised bed and its LED strip, and the bathroom's motion-sensing light. Also live room status, scenes (room, lights, all on / off), temperature and humidity, weather, bookmarks, encrypted shared lists, photo and video upload, a live doorway camera, and a live LAN scanner that checks the tracked hosts for SSH and ping.
- **LED pixels.** Pis drive LED walls, grids and strips through Fadecandy or WS2812 directly, with C++ effects (text scroll, spectrum, maze, noise, orbits and more), video playback on the pixels, shuffle, and per-host brightness, kept in sync from one control.
- **Raspberry Pi modules.** A shared Pi base plus one small module per Pi: AirPlay and Bluetooth audio receivers, LED hosts, IR and motion-recording cameras with cloud upload, and the bedroom hub's serial bridge. Each Pi runs a read-only root filesystem so it survives losing power, recovers its own Wi-Fi, falls back to a setup hotspot, and compiles its C/C++ programs on install.
- **Microcontroller firmware.** Pico W lamp and light controllers and an HVAC thermostat (SmartThermo) with their own HTTP API, and Arduino/AVR boards for the bedroom hub, bed motor, switched outlets, the integrated amp and an IR remote hub.
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
  │                                     ║      └─ LED hosts (opts.json)              │
  │  host tmpfs /mnt/ramdisk ◄══════════╝                                            │
  │      ▲ file-monitor.sh → apicmds.sh (backups, regions, start/stop VMs)           │
  │  files LXC 101 (SMB) · mgmt LXC 102 · legacy LXC 104 · media LXC 108             │
  │  router VM 100 · appletalk VM 103 · xana VM 105 · unifi VM 107                   │
  │  ZFS: rpool (boot) · tank/datastore (data, mirrored) · scratch SSD · ext. backups │
  └──────────────────────────────────────────────────────────────────────────────────┘
       Pis: brpi · ledwall · ledgrid · ktchpi · doorway · homepi · btpi · lrpi
       Microcontrollers: BathroomLight, LivingRoomLamp, SmartThermo (Pico W) · Bedroom hub, amp, IR hub (AVR)
```

## Folder map

Every top-level folder is one of four kinds. `login.sh` tells them apart by the marker file in the folder: `lxc.conf`, `qemu.conf` or `pc.conf` means a server ("nonpi"), anything else is a Pi module.

| Kind | Folders | Layout | Deployed by |
| --- | --- | --- | --- |
| Pi base | `rpi/` | Programs, `config/` (installer, systemd units, system configs), `effects/` (LED programs) | Copied to every Pi first, to `/opt/rpi` |
| Pi module | `brpi` `btpi` `doorway` `homepi` `ktchpi` `ledgrid` `ledwall` `lrpi` | `boot.sh`, `config/` (`installer.sh`, `brand.txt`, `config.txt`, other configs), extra programs or firmware | Overlaid onto `/opt/rpi` after the base, with `config/` sent to `/opt/rpi/config/module/` |
| Server (LXC / VM / PC) | `automate` `files` `legacy` `media` `mgmt` (LXC); `appletalk` `xana` (VM); `pve` (the Proxmox host); `brpc` (a physical PC) | `config/installer.sh` plus the files it installs; `lxc.conf` / `qemu.conf` / `pc.conf` marker | Tarred to `/tmp/config` and `installer.sh` run |
| Not deployed | `shared/` (Automate site, deployed with `automate` and every Pi), `wkst/` (this workstation, `install.sh` run by hand), `router/` and `unifi/` (only their VM backups, written by the backup wizard's VM backup) | | |

`login.sh` refuses `rpi`, `wkst`, `shared`, `sources`, `login`, `logon` and `.ssh` as host names.

## Network and hosts

DNS domain `home`, LAN 192.168.1.0/24, gateway and DNS the router at 192.168.1.1. Addresses known from the code:

| Host | Kind | Address | What it is |
| --- | --- | --- | --- |
| `router` | VM 100 | 192.168.1.1 | The router, with two NICs passed through over PCIe (`hostpci0/1`); login is `admin@router`, not root. Configs in `pve/network/` (pfSense, EdgeRouter, UniFi backups) |
| `files` | LXC 101 | .4 | Samba file server (SMB2+): shares Data, Media, Scratch, Cameras, Games, MP3s, Regions, External, GSync (Google Drive, mounted on `pve`) |
| `mgmt` | LXC 102 | .7 | Management box: holds the SSH keys (`mgmt/keys/`, git-ignored) and runs `login.sh`; mounts the Projects share read-only at `/mnt/Projects` |
| `pve` | Proxmox host | .8 | Hypervisor; 4 NICs bonded 802.3ad into `vmbr0` |
| `appletalk` | VM 103 | | Netatalk 4 AFP server (built from `netatalk-4.4.3.tar.xz`) for vintage Macs, over AppleTalk (DDP) and TCP; started at boot by `afp-start.service` |
| `legacy` | LXC 104 | .19 | Samba with `LANMAN1` minimum protocol, for old clients; started on demand from the website |
| `xana` | VM 105 | | Debian desktop over TigerVNC: Firefox in Firejail, Transmission behind a PIA WireGuard tunnel with kill switch and watchdog, retro themes |
| `automate` | LXC 106 | .16 | The Automate website and node API |
| `unifi` | VM 107 | | UniFi network controller |
| `media` | LXC 108 | .11 | Jellyfin, plus `mp3sync` (C++, `mp3sync.cpp`): watches the music library with inotify and mirrors it as MP3s into the MP3s share |
| `brpc` | PC | .17 | Bedroom Lenovo ThinkCentre: its own lighttpd + node API (`server-min.js`) and `webapi.sh` so Automate can sleep it; woken by wake-on-LAN |
| `brpi` | Pi | .15 | Bedroom Pi (wired, Wi-Fi off): AirPlay, serial bridge to the bedroom hub and the amp, relax sounds |
| `ledwall` | Pi | .12 (`lrxmit` in the API) | LEDwall: the 93-pixel LED ring (Fadecandy) and the living room's command host. Its `custom.js` serves `extcom` (living room stereo codes over serial on `/dev/ttyAMA0`), the white LED, the front button and the desktop PC power relay (`pc_power`) |
| `ledgrid` | Pi Zero | | WS2812 grid driven directly (`opcserver`) |
| `ktchpi` | Pi | .22 | Kitchen Pi (formerly `ledstrip`): 3×60 LED strips (Fadecandy), "Kitchen" in `ledHosts` in `shared/nodeapi/opts.json` |
| `doorway` | Pi | | IMX462 IR camera: `rtspcam` (RTSP on :8554/cam), `minimotion` recorder, `ldrdetect` day/night; clips go to the files server's Cameras and GSync (Google Drive) shares at once, over autofs; a clip neither share took is kept instead on a 2 GiB ext4 partition at the end of the SD card (label `camfallback`, `fallback_dir = /mnt/fallback/minimotion`), made once by `rpi/fallbackpart.sh` from the doorway installer when that space is free (marker `/etc/fallbackpart.enabled`) and repaired and mounted by `boot.sh`; `camcleanup.sh` keeps `GSync/Cameras/Doorway` under 14.5 GB and the fallback folder under 1.9 GB with its own 0.5 GB reserve, oldest first (settings in `doorway/config/camcleanup.conf`) |
| `homepi` | Pi | | Family-home camera hub: `motion` with two cameras, uploads via rclone; `camcleanup.sh` keeps the flash drive and Google Drive camera folders under their limits (settings in `homepi/config/camcleanup.conf`) |
| `btpi` · `lrpi` | Pi | | Audio receivers: AirPlay (shairport-sync) and Bluetooth (BlueALSA) into a stereo; `btpi` adds Bluetooth to a vintage stereo, `lrpi` (formerly `stereo`) is the living room's, branded "Living Room" |
| `desk` | PC | .14 | Living room desktop, pinged by `wkststate` |
| `lrmlamp1` · `bthrlight1` | Pico W | .18 · .21 | Living room lamp, bathroom light (firmware in `pico/`) |
| `lrthermo1` | Pico W | .3 | SmartThermo, the living room's HVAC thermostat (firmware in `pico/SmartThermo/`) |
| `wkst` | Workstation | | This machine: mounts the `files` SMB shares as `/mnt/SMB/<share>` with an autofs indirect map (`auto.files`) and its SFTP mount on `/mnt/SFTP` with a direct map (`auto.sftp`), both listed in `files.autofs`, detached while `files` is down by `share-watchdog.sh` (so this repo, `/mnt/SMB/Data/Projects/ProOS`, is the Data share). Not a direct map on `/mnt/<share>`: that stacks each share on an autofs mount at the same path, and KDE's trash takes the share for the autofs mount and refuses to trash on it |

The LED host list (`ledHosts` in `shared/nodeapi/opts.json`) is the living list of LED hosts the website talks to; the other device addresses are in `IP_ADRS` in `shared/nodeapi/controller.js`.

## Deployment: `login.sh`

`login.sh` runs on `mgmt`, where the repo is at `/mnt/Projects/ProOS` (its `ROOTDIR`), and needs `/opt/deploy` as a scratch folder. A host's key (`mgmt/keys/<host>.rsa` for servers, `rpi.rsa` for all Pis) has a passphrase when the host's folder holds an empty `ssh.pwd` marker file (`<host>/ssh.pwd`; for the Pi key, `rpi/ssh.pwd` or any Pi module's); all marked keys share one passphrase. Marked keys go through one `ssh-agent` per terminal session (keyed by the calling shell's PID, socket under `$XDG_RUNTIME_DIR/ssh-agents/`, or `/dev/shm/proos-<uid>/` without it): the first marked login in a terminal adds every marked key in one `ssh-add` call, so a single `Enter passphrase:` prompt unlocks them all (the script is its own `SSH_ASKPASS` prompter, run through bash by a two-line `askpass` wrapper it writes into the session folder, so no key path is shown; Enter skips a key for the rest of the session), and later runs reuse the agent. Unmarked keys are used straight from the file. Each connection uses `-o IdentitiesOnly=yes -i` with the host's key (its public key taken from the agent when unlocked), so ssh never offers the other loaded keys. The agent is removed by `login clearkeys`, by the `EXIT` trap in `mgmt/config/profile_ben`, when its shell is gone, or after 8 h; the passphrase itself is never stored. It checks port 22 with `nc`, then:

| Command | Does |
| --- | --- |
| `login <host>` | SSH shell as root (`admin` for `router`) |
| `login <host> sync` | Deploy: server → `DEPLOY_SERVER`, Pi → `DEPLOY_PI` |
| `login <pi> reset` | Deploy a Pi after deleting `/etc/rpi-conf.done`, so the installer's first-run phase runs again |
| `login <module> init <host>` | Deploy module `<module>` onto the Pi currently reachable as `<host>` (renames it to `<module>`) |
| `login <host> update` | Updates the host's packages: `apt-get update; apt-get upgrade` on a server, `rpi update` on a Pi. Given a host list (`login pve,files,automate update`), every host updates at once in its own tile |
| `login <host> reboot` | Reboots a server or a Pi after a 3 s Ctrl+C countdown (a Pi keeps its read-only or read-write mode); `login files,legacy reboot` reboots each in its own tile |
| `login <pi> <cmd>` | Runs `rpi <cmd>` on the Pi (see `rpi/rpi.sh`) |
| `login <host1>,<host2>,… [<cmd>]` | Opens one tmux tile per host (a new window when already inside tmux), each running `login <host> [<cmd>]` with the same `<cmd>` (not `init`). The keys are unlocked once before the tiles open and the tiles share that terminal's agent (`PROOS_AGENT_DIR`); each tile waits for a key before it closes. `mgmt` installs `/etc/tmux.conf` with `mouse on`, so a click selects a tile |
| `login clearkeys` | Forget this terminal's unlocked keys (stops its agent) |
| `login cmds` · `login rmtmp` | Command cheat sheet · delete deploy temp files and stop every session agent |

**A new Pi.** Start from stock Raspberry Pi OS with root SSH login and password authentication enabled, then run `login <module> init <host>`: the first connection falls back to the root password, and the installer replaces root's `authorized_keys` with `rpi/config/authorized_keys` (the public half of `mgmt/keys/rpi.rsa`, git-ignored) and installs an `sshd_config` that allows root by key only (`PermitRootLogin without-password`, `PasswordAuthentication no`).

After a deploy a menu offers re-deploy, update packages, reboot (a Pi reboots into read-only mode with `rpi ro`) or a shell.

**Server deploy.** Copies `<host>/config` to a temp folder (for `automate`, also `shared/html` and `shared/nodeapi` into it), tars it, `scp`s it to `/tmp`, untars to `/tmp/config` and runs `/tmp/config/installer.sh` as root. Every server installer follows the same shape: apt packages, users (`tank` uid 1000 and group `shared` gid 1000, which the LXC idmaps pass through unshifted), sshd config and `authorized_keys`, `profile`, `sysctl.conf` (the VMs only; an LXC installs its `config/sysctl-net.conf` as `/etc/sysctl.d/90-net.conf` and deletes `/etc/sysctl.conf`, and `pve` uses files in `/etc/sysctl.d/`), service files started at boot with `systemctl enable` (each unit has an `[Install]` section), then deletes `/tmp/config`. No host boots through `rc.local`: every server installer deletes `/etc/rc.local`, `/etc/systemd/system/rc-local.service` and its `multi-user.target.wants` link.

**Site icons.** The Automate site's Font Awesome sheet and fonts (`styles/icons.min.css`, `fonts/fa-*.woff2`) are not in the repo; they live only in the cache `/tmp/proos-fontcache` on `mgmt` (the repo is read-only there). Before every deploy of the site (`automate` and every Pi), `login.sh` checks the cache against the site with `shared/fontawesome/build-icons.py --check`, builds into it with `build-icons.py --out` when a page names an icon it lacks (or after a reboot cleared `/tmp`), and copies the cached files into the deploy's copy of the site (a work folder under `/opt/deploy` for a Pi), each step printed as an `Icons:` line. The cache stays after a deploy, so later deploys reuse it, and a lock file in it makes parallel deploys (tmux tiles) take turns. A failed build sends the cache's older build with a warning (icons added since show blank); with no build at all, or a cache that cannot be written, the site goes with every icon blank, also with a warning. The build needs `python3-fonttools` and `python3-brotli`, which `mgmt/config/installer.sh` installs.

**Pi deploy.** If the root filesystem is an overlay it first runs `rpi rw` and waits for the reboot. Then it rsyncs, in order: `rpi/` → `/opt/rpi`, a copy of `shared/html/` with the icons from the cache → `/opt/rpi/config/html/`, `shared/nodeapi/` → `/opt/rpi/config/nodeapi/` (replacing what an earlier deploy left there), `<module>/` → `/opt/rpi/` (excluding `config`, `photos` and `sources`), and `<module>/config/` → `/opt/rpi/config/module/` (replacing what an earlier deploy left there), writes the module name to `/opt/rpi/config/hostname` and runs `/opt/rpi/config/installer.sh`.

Because the module is copied over the base, **a module's files replace the base's files of the same name**: `<module>/boot.sh` replaces `rpi/boot.sh`. The one exception is the module's `config/`, which lands beside the base's as `/opt/rpi/config/module/`, so its `installer.sh` and `config.txt` do not replace the base's.

## Raspberry Pi platform (`rpi/`)

Requires Raspberry Pi OS on Debian **trixie**; the installer refuses anything else.

### Installer (`rpi/config/installer.sh`)

1. **First run only** (no `/etc/rpi-conf.done`): locale, time zone, `pi` user, a long apt package list (build tools, GStreamer, ffmpeg, Bluetooth, node, lighttpd, motion; no X11 desktop), purges cron, logrotate, swap, PulseAudio/PipeWire, cups and others, deletes old `rpi-*` units and `/etc/camcleanup.conf`.
2. **Every run**: read-only boot partition, `config.txt`, sshd and key, `sysctl`, NetworkManager, journald (to RAM), systemd units from `config/systemd/` into `/etc/systemd/system/`, `/etc/netmode.sh`, `/etc/netwatchdog.sh` and `/etc/preinit.sh`.
3. **Node API**: moves `rpi/controller.js`, `rpi/ledparser.js` and `rpi/custom.js` (or the module's `config/custom.js`) over the shared ones in the staged API, then installs it into `/opt/nodeapi` with `api-install.sh`, which copies new and changed files, deletes files no longer deployed and prints a table of each (`npm install` only when `package.json` changed, tracked in `.deps.hash`).
4. **Website**: rsyncs the staged html into `/var/www/html` (keeping `ram/`), sets `PAGE_NAME` in `opts.js` to the module's `brand.txt` name (so `main.js` titles the page with it and draws a Pi page, not the server's), minifies (all but `opts.js`), and gives `www-data` sudo on `servercmds.sh` and `relaxrun.sh` only.
5. Bluetooth, ALSA, Samba USB share, udev USB automount, motion defaults, hostname and `/etc/hosts`.
6. **`config/cleanup.sh`**, once per Pi (skipped while `/etc/rpi-cleanup.done` exists; delete it and deploy to run it again), so the system fits an 8 GB card: swaps `libopencv-dev` for `libopencv-core-dev` + `libopencv-videoio-dev`; purges the X11 desktop, the `arduino` apt package and its AVR toolchain, USB Wi-Fi dongle firmware, the `v6` kernels, every kernel but the running and newest, and all kernel headers; deletes `arduino-cli` and `~/.arduino15` unless the module's `config/installer.sh` installs it (only `brpi` does); installs `config/dpkg-nodoc.conf` as `/etc/dpkg/dpkg.cfg.d/01-nodoc` and deletes docs, man pages and non-English translations; clears the apt lists and npm cache.
7. **`config/compile.sh`**: builds the C/C++ programs (below).
8. First run: enables `ssh avahi-daemon proinit NetworkManager timesyncd`, disables the rest (lighttpd, bluetooth, smbd, hostapd …), touches `/etc/rpi-conf.done`.
9. Runs `/opt/rpi/config/module/installer.sh` (the module's own installer, from `<module>/config/installer.sh`), then **deletes `/opt/rpi/config/module`**; restarts lighttpd and nodeapi; links `/usr/bin/rpi` → `rpi.sh`; deletes `/opt/rpi/config`.

### Boot sequence

`proinit.service` (after `network-online.target`) runs `/etc/netmode.sh boot`, then `/opt/rpi/boot.sh` — the module's own boot script. Module boot scripts start only the services that module needs, passing settings through `systemctl set-environment` (e.g. `rpi_ledtype`, `rpi_ledmaxbright`, `rpi_gpipin_1`, `rpi_imx*`), which the `rpi-*.service` units read. Nothing module-specific is enabled at boot; services are started by `boot.sh`.

### Read-only root

Pis run with an OverlayFS root, so writes go to RAM and the Pi can be unplugged safely (the SD cards used to corrupt when a Pi lost power). `rpi rw` / `rpi ro` reboot into read-write / read-only (via `raspi-config nonint disable_overlayfs`/`enable_overlayfs`); the boot partition is remounted rw only for the moment it is written. Anything that must survive a reboot on a Pi has to be written in rw mode or to the boot partition / a USB drive.

### Networking (`rpi/config/netmode.sh`, `rpi/config/netwatchdog.sh`)

`netmode.sh` takes the mode commands `boot`, `client`, `apd`, `delwifi`, and the status commands `inapd` and `inclient` (exit 0 when that mode is active) and `ssid` (prints the client network name), which change nothing. Wi-Fi credentials live on the boot partition (`/boot/firmware/wpa.conf`, `NAME|$|PASSWORD`, written by the website's Wi-Fi setup through `servercmds.sh confwpa`). The client profile (`RPiWiFi`) connects on 2.4 GHz only: on 5 GHz ktchpi stopped receiving broadcasts, so other hosts lost it once their ARP entry expired. Flags on the boot partition: `disable.wifi`, `apd.enable` (start as hotspot `RPiHotspot`). With no known network at boot the Pi falls back to hotspot mode with a captive DNS (`captive.conf`). `rpi-netwatchdog.service` runs `netwatchdog.sh` (installed as `/etc/netwatchdog.sh`, started from a module's `boot.sh`), which waits out access point outages and recovers a dropped Wi-Fi connection in stages (rejoin, driver and firmware reload, reboot) without writing to disk; it runs `/etc/netmode.sh` as a command (`client` to rebuild the connection, the status commands for the mode and network name) rather than loading its functions, and its tunables are at the top of the script.

### `rpi` command (`rpi/rpi.sh`)

`ro`, `rw`, `update`, `expand` (refused once `/etc/fallbackpart.enabled` exists), `net`, `wifiscan`, `wifistats`, `togglenet`, `client`, `apd`, `apdonboot`, `enablewifi`/`disablewifi`, `delwifi`, `resetbt`, `restart-alsa`, `temp`, `jl`/`jh` (journal), `running`/`active`/`timers`/`loadtimes`. The website's Pi options call these through `servercmds.sh`, which writes each command's output to its own file, `/var/www/html/ram/<command>.out.save.txt` (`wifiscan` → `wifiscan.out.save.txt`, `confwpa` and `showwpa` likewise), emptied first so it holds only the newest run, with two runs of one command taking turns under a lock on the file, as `file-monitor.sh` does on `pve`; the run's output sits between the same `@START` / `@END` lines (the `@END` written by an `EXIT` trap, also when the script is stopped), and the Server window's command output window streams it through `/api/svrcmd`, as on the server. A Pi has no Clear Log.

### Pi-side node API

The Pis run the same `shared/nodeapi/server.js`, but `rpi/controller.js` replaces the server's command dispatch: `server <cmd>` → `sudo servercmds.sh <cmd>` (allowed 120 s, as the page follows its output file rather than waiting on the answer), `confwpa` → Wi-Fi setup, everything else → `exeCustom()` in `custom.js`. The base `rpi/custom.js` is a stub; a module with real commands ships `config/custom.js`, which its `config/installer.sh` copies over it:

- `brpi/config/custom.js`: serial via `ztermcom` (one FIFO queue, at most 8 waiting) to the amp on `/dev/ttyUSB0` and the bedroom hub on `/dev/ttyAMA0` (`hwcom`, `usbcom`), amp power/inputs/volume, `bed_leds_hex_set`, relax sounds (`relaxSounds` in `opts.json`, `relaxrun.sh` → `rpi-relaxloop.service`), activity LED flash.
- `ledwall/config/custom.js`: `extcom` (serial), `front_btn` (LED shuffle), white LED, `pc_power`.

`rpi/ledparser.js` replaces the server's LED fan-out with the local effect runner: it spawns one effect binary from `/opt/rpi/effects`, plays videos through `mp4toleds`, and sets brightness through the proxy. Shuffle picks from `ledShuffle` in the API's `opts.json`. Each command also records what the pixels show, which `fc_status` (`/api?action=leds&arg=fc_status`) answers as JSON with the brightness: `{ effect, state, color, brightness }`, state `off` (stopped, or black), `active`, `paused` (also an effect that ended on its own) or `color` (RRGGBB); the server's Pixels window reads it.

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
| `minimotion` | `rpi-minimotion` | | Motion recorder: reads the RTSP stream, records in `/mnt/ramdisk`, copies clips to every `copy_to` folder at once and logs each one that fails; a clip no `copy_to` folder took goes to `fallback_dir`, or with `fallback_dir = off` is only logged and the clip still leaves the RAM disk; a clip that stays there (or that an earlier run left) is deleted after `temp_max_age`, or oldest first while the RAM disk has less than `temp_min_free` free, so it never fills (`doorway/minimotion.conf`) |
| `ztermcom` (`ztermcom.c`) | CLI | serial | Framed serial request/response to Arduino hubs |
| node API · lighttpd | `nodeapi`, `lighttpd` | :3000 · :80 | Same as the Automate server |

`ledtype.txt` (written by a LED module's `config/installer.sh` under `## Set LED Type`) names the layout: `93ringinv` (ledwall), `3x60strips` (ktchpi), `16x16grid` (ledgrid). The step after it, `## Enable Pixel Menu in Website`, sets `LED_PI` to true in the deployed `/var/www/html/opts.js`, so the Pi's own page draws the Pixels group and the LED text scroller; a new LED module needs both steps.

### Compiling (`rpi/config/compile.sh`)

C/C++ sources are compiled on the Pi at install time. `build()` compiles only when the source's sha256 differs from its archived copy in `/opt/source-backup`, the binary is missing, or a shared dependency changed (`opcutils.*` / `httpapiserver.*` for the servers, `ledlayout.*` / `opcclient.*` for the effects); the source is then **moved** into `/opt/source-backup`. A new program needs a `build`/`effect`/`tool` line here. The ws281x library is built once into `/usr/local`.

## Microcontroller firmware

| Folder | Board | Talks to | Role |
| --- | --- | --- | --- |
| `pico/BathroomLight/` | Pico W | HTTP :4000 | Bathroom light + C4002 mmWave sensor; API reference `.md` alongside |
| `pico/LivingRoomLamp/` | Pico W | HTTP :4000 | Living room lamp (switched lamp + dimmable LED, rotary encoder in a remote box on a 3.5 mm TRS cable, its push button sharing the A and B lines through a BAT54C; wiring in `remote-wiring.svg`); derived from BathroomLight |
| `pico/SmartThermo/` | Pico W | HTTP :4000 | HVAC thermostat (furnace + central AC): W1 / Y1 / G relays, DHT22, ST7735R TFT display, every interlock and lockout from the HVAC control reference, watchdog; API reference `.md` alongside; derived from BathroomLight |
| `automate/devices/InfraredRemoteHub/` | Arduino | serial | Sends IR codes to the stereo's preamp, DAC and sub amp |
| `brpi/BedroomHub/` | AVR | brpi `/dev/ttyAMA0` | Bedroom hub #1: relays, temperature/humidity (DHT), two external serial ports |
| `brpi/BedroomHubExt/` | ATmega328P | hub serial #2 | WS2812 bed strip, switched outlets, bed motor (EAGLE board files alongside) |
| `brpi/BedroomPowerExt/` | AVR | serial | 2×120 VAC + 12 VDC switched controller |
| `brpi/IntegratedAmp/` | Arduino Uno | brpi `/dev/ttyUSB0` | Integrated amp controller; flashed from the Pi with `brpi/update-amp.sh` (`update-bedhub.sh` for the hub) via `arduino-cli` |

Pico firmware is built with the arduino-pico core (`build/rp2040.rp2040.rpipicow/`, git-ignored); Wi-Fi credentials are in `creds.h` (git-ignored). Pico commands use the `http://<host>:4000/api?<cmd>,,<value>~` form, errors as text under HTTP 200.

## Proxmox host (`pve/`)

`pve/config/installer.sh` sets up the host: no-subscription apt source, NIC name pinning (`50-pve-nic*.link`) and the bonded `interfaces`, IOMMU / PCIe passthrough (`grub`, `blacklist.conf`), swap moved to a swapfile on the scratch SSD (turned on from `fstab`, swappiness 10 in `90-swappiness.conf`), `fstab`, the sensor drivers `nct6775` and `coretemp` loaded at boot (`sensors-modules.conf` → `/etc/modules-load.d/sensors.conf`), `proos-boot.service` (CPU governor `performance`, `pve-firewall stop`), postfix mail relay (`sasl_passwd`, git-ignored), apcupsd (APC SC620 UPS), smartd, lm-sensors, ZFS auto-snapshots on `tank/datastore` (hourly/daily/weekly/monthly cron, no 15-minute), scrub/trim cron, `sys-check` and `kernelclean` tools, the `tank-backup.sh` and `pve-backup.sh` backup scripts, and the services below. `pve/qemu.conf` is only a placeholder so `login.sh` treats `pve` as a server. `pve/README.txt` holds the ZFS runbooks (new mirror, encrypted backup drive, replacing a data or boot drive) and `pve/zfsman` is a ZFS helper.

Storage: `rpool` (boot, ZFS), `tank` mirrored with `tank/datastore` at `/mnt/datastore` (`data`, `media`, `.regions`), a scratch SSD at `/mnt/pve/scratch` (camera footage, downloads, MP3s, uploads, swap), encrypted external backup pools listed in `bkpdrives.txt` attached under `/mnt/extbkps`.

### Website → host commands

This is how the Automate Server window controls the host:

1. The host mounts a 128 MB tmpfs at `/mnt/ramdisk` (owner `tank:shared`, setgid), bind-mounted into the automate LXC as `/var/www/html/ram`.
2. The node API's `server <name>` command creates an empty `ram/<name>.txt`. The Server window runs it through `GET /api/svrcmd` (`shared/nodeapi/svrcmd.js`), which follows the command's output file and streams it to the page's command output window; users asking for a command already running share its one run, and a command whose output file is in use (the region commands) waits for it.
3. `file-monitor.service` (`pve/config/file-monitor.sh`) watches the ramdisk with inotify, claims each `*.txt` trigger atomically into `spool/`, and runs `/usr/bin/apicmds.sh <name> /mnt/ramdisk` in the background (at most 4 at once). Each command's output goes to its own file in the ramdisk, picked by `outputRoutes` at the top of the script: `<name>.out.save.txt` (`startxana` → `startxana.out.save.txt`) for a command with no row there. A row (`<command glob>|<file>|<mode>`) names another file and its mode: every region attach and detach (`mnt_*_region`, `unmnt_*_region`, `unmnt_all`) shares `regions.out.save.txt`, and the backup wizard's `bkp_*` commands go to `bkplog.save.txt` (see *Backup jobs*). In `replace` mode, the default, the file is emptied before the command runs, so it holds only the newest run, and commands sharing the file wait for each other under a lock on it and run one at a time (the command itself gets the lock's descriptor closed, so a VM, container or mount it starts does not keep holding it); `append`, the backup wizard's, adds to the end with no emptying and no waiting, as a cancel has to reach a running job's log. A new command needs no row unless it shares a file or appends. Data files are on its `IGNORE` list (`*.save.txt`, `*.lock`, `*.tmp`): the command output files, the backup wizard's `bkpstatus.save.txt` and `bkplog.save.txt` and the passfiles `bkppwd.save.txt` and `fusearch.save.txt` are covered by `*.save.txt`, so a new data file kept in `ram/` is named `<name>.save.txt` and needs no entry; any other `.txt` name is consumed as a command. `pve/config/installer.sh` restarts the service on its new script unless a job holds a lock.
4. In `replace` mode the run's output sits between `@START|<name>|<epoch ms>` and `@END|<name>|<exit code>|<epoch ms>` lines, written by `DISPATCH`, by which `/api/svrcmd` finds the run it asked for and knows when it has ended; the website can also read the file through `/api/read?file=<name>.out.save` (the file name without `.txt`). There is no shared log.

`apicmds.sh` commands: the backup wizard's jobs, every `bkp_*` command (handed to `/usr/bin/tank-backup.sh`, `pve/config/tank-backup.sh`; see *Backup jobs*), region mounts (`mnt_/unmnt_arch_region` gocryptfs Archive, `mnt_/unmnt_ram_region`, `mnt_/unmnt_snap_region`, `unmnt_all`), `pve_listsnaps`, `start/stop` for `legacy` (LXC 104), `xana` (105, plus `restorexana`), `unifi` (107), `atalk` (103), `rebootrouter`, `syslog` (`sys-check`). A command handed to its own script is `exec`ed by `RUN_SCRIPT` with the same two arguments (`<name> <RAM disk>`), so each script can also be run by hand, e.g. `tank-backup.sh bkp_status /mnt/ramdisk`. A new server command needs a case here and an option in `shared/html/server.js`.

### Backup jobs (`tank-backup.sh`)

The website's backup wizard (Server > Options > Backup) runs every backup as a job on the host: attaching the external backup pools, backing tank up to them, backing the guests up, and detaching the pools. It runs apart from the Server window's command output window: the API (`shared/nodeapi/backup.js`, `/api/backup/…`) drops a `bkp_*` trigger, file-monitor appends the command's output to `ram/bkplog.save.txt` (its `append` row in `outputRoutes`), and the job writes its own log there.

The pools in `bkpdrives.txt` (installed as `/opt/bkpdrives.txt`; `usb*` and `hdd*` names, `tank`, `datastore` and `rpool` refused) are encrypted ZFS pools, each with one `<pool>/extbkp` dataset mounted at `/mnt/extbkps/<pool>` (set up as in `pve/README.txt`).

| Command | Job | Does |
| --- | --- | --- |
| `bkp_attach` | `attach` | The API writes the passphrase to `ram/bkppwd.save.txt` (mode 600) first. Each connected pool (`zpool import` lists it) is imported, its key loaded and its dataset mounted, one step per pool; one whose key or mount fails (a wrong passphrase) is exported again, so nothing stays half attached. Pools not connected are only listed. The passphrase is shredded when the job ends, even when refused |
| `bkp_standard` · `bkp_checksum` | `standard` · `checksum` | One step per share of each attached pool: `data` → `Data`, `.regions` → `.Regions` and, on an `hdd*` pool, `media` → `Media` (a `usb*` pool skips disk images, Games and the Archive region's files), `--checksum` for `bkp_checksum`, rsync's `--info=progress2` turned into progress events. Only a pool ZFS has mounted at `/mnt/extbkps/<pool>` (its `mounted` and `mountpoint` properties and the mount on that folder) is written; a share is synced only when its sanity path is on tank (`Projects/ProOS`, `.regions/DB`, `media/Music`). A pool's date goes into `/mnt/extbkps/status.txt` only when every share's rsync answered 0, or 24 (files vanished, a warning). The LEDwall's white LED is lit meanwhile |
| `bkp_standard_eject` · `bkp_checksum_eject` | as above | The same, then detaches the pools in the same job, unless it was cancelled |
| `bkp_vms` | `vms` | `pve-backup.sh`: a vzdump of each guest, one step each (a KVM guest's vzdump percent as progress) |
| `bkp_detach` | `detach` | Every imported backup pool, mounted or not, is unmounted, its key unloaded and exported, one step each; a busy one stays attached |
| `bkp_cancel` | | Stops the running backup or VM backup: kills its worker (the rsync, or `pve-backup.sh` and its vzdump) by the PID in `ram/bkpworker.pid`. The pools stay attached and the pools finished before keep their dates |
| `bkp_status` | | Rewrites `ram/bkpstatus.save.txt`, printing nothing |

One job runs at a time: `ram/bkpjob.lock` holds `<job>|<pid>`, and a job asked for meanwhile is refused (`@REFUSED` in the running job's log; the API refuses it first). Every job and `bkp_status` rewrite `ram/bkpstatus.save.txt`: `STATE|<idle or job>`, `UPDATED|<time>`, then `POOL|<name>|<state>|<last backup>|<shares>` per pool, the state `attached`, `unmounted` (imported, not mounted), `connected` or `absent`.

Each job begins `ram/bkplog.save.txt` afresh (a new file, so the API can tell a new job's log by its inode) and logs plain text lines and event lines, which the wizard draws from. Their fields, split by `|`, are listed at the top of `tank-backup.sh`:

| Event | Meaning |
| --- | --- |
| `@JOB\|<job>\|<epoch ms>` | The first line |
| `@PHASE\|<phase>` | `attach`, `backup`, `record`, `detach` |
| `@STEP\|<n>\|<count>\|<group>\|<item>` | A step of the phase starts: pool and share, guest id and name, or pool and `attach` / `detach` |
| `@PROGRESS\|<step %>\|<overall %>\|<transferred>\|<rate>\|<eta>` | At most once per percent or every 5 s; step % is `-` while the worker gives none (an LXC's vzdump) |
| `@STEP_END\|<n>\|<ok, warn, failed or cancelled>\|<detail>` | The step's outcome |
| `@POOL\|<pool>\|<outcome>` | `attached`, `already`, `wrongpass`, `failed`, `done`, `incomplete`, `skipped`, `detached`, `busy` |
| `@CANCEL\|<epoch ms>` · `@REFUSED\|<job>\|<running>` | Cancel asked for; a job refused |
| `@DONE\|<job>\|<ok, failed or cancelled>\|<summary>` | The last line (`failed`, `interrupted` when the job was killed) |

### Google Drive mount (`rclone-gsync`)

`rclone-gsync.service` mounts the rclone remote `gsync` at `/mnt/rclone/gsync` on the host as `tank:shared` (the unit is based on a plain `rclone mount` service, with its VFS cache on the scratch SSD at `/mnt/pve/scratch/rclone`). Because `tank` owns the datastore, the AppArmor profile `pve/config/apparmor-rclone-gsync` (installed as `/etc/apparmor.d/rclone-gsync`) confines rclone and its `fusermount` to the config `/etc/rclone/rclone.conf`, the cache and the mountpoint. systemd's own sandboxing cannot do this: its namespacing options would hide the mount from the host, and its seccomp options imply `NoNewPrivileges=`, which stops the setuid `fusermount`. `pve/config/fuse.conf` adds `user_allow_other` so the mount can be shared. The files LXC gets `/mnt/rclone` through an `lxc.mount.entry` rbind added by hand to `/etc/pve/lxc/101.conf` and shares it as GSync; running rclone on the host also keeps a vzdump freeze of the LXC from deadlocking on the FUSE mount. The service waits for its config, and the setup steps are in `pve/README.txt`.

### Persistent state and backups

- **Website state** (`bookmarks.save.txt`, `lists.save.txt`, their lock and password files) lives only in the RAM disk. `hourly.sh` (`hourlyjob.timer`, and `hourlyjob-sd.service` on shutdown) copies them to `/mnt/datastore/data/Documents/` (`Bookmarks.txt`, `Lists.txt`, keeping a `.bak`), and restores them into the RAM disk when they are missing after a reboot.
- **The VM backup** (the backup wizard's `vms` job; `pve-backup.sh pve_vmsbkp /mnt/ramdisk` by hand) vzdumps every guest (`guestList` and `BACKUP_GUEST` in `pve-backup.sh`), copies each guest's archives and logs (`vzdump-*`, git-ignored) into its own folder (101 → `files/`, 104 → `cifs/`; the router and UniFi, which have no host folder, into `router/` and `unifi/`, made on first run), and **overwrites `<host>/lxc.conf`, `appletalk/qemu.conf`, `xana/qemu.conf`, `router/vzdump-qemu-100.conf` and `unifi/vzdump-qemu-107.conf` in this repo** from `/etc/pve`. Those files are generated snapshots of the live config: edit the guest in Proxmox, not here. The router's and UniFi's are not named `qemu.conf`, so `login.sh` does not take their folders for servers. `pve/vmbkps/` holds older backups from before.

## Conventions across the repo

Code style rules (naming, comments, shell, JavaScript and CSS) live only in [`CODESTYLE.md`](CODESTYLE.md), imported at the end of this file. This section covers how the repo is organised and worked on.

- Camera cleanup (`rpi/camcleanup.sh`, `rpi-camcleanup.timer`) takes every setting from `/etc/camcleanup.conf`, which a module's `config/installer.sh` copies from its `config/camcleanup.conf` (`doorway`, `homepi`); a Pi without one never runs it, and an installer reset deletes it before the module's installer runs. A location (flash drive, Google Drive, the camera fallback partition) is cleaned only when its `*_PATH` is set; the fallback partition has its own reserve (`FALLBACKPART_RESERVE_GB`), the others share `FREE_SPACE_RESERVE_GB`.
- A new Pi service is a `rpi-<name>.service` in `rpi/config/systemd/`, not enabled, started from the module's `boot.sh` with settings passed by `systemctl set-environment`.
- A new Pi module is a folder with `boot.sh` and `config/{installer.sh,brand.txt,config.txt}`, the same `config/installer.sh` layout as a server (its installer runs after the base's, from `/opt/rpi/config/module/`); `brand.txt` becomes the page title, AirPlay/Bluetooth/Samba name.
- A new server is a folder with `config/installer.sh` and a marker file; add its key as `mgmt/keys/<name>.rsa`, plus an empty `<name>/ssh.pwd` if the key has the shared passphrase.
- A server starts what it needs at boot with `systemctl enable`, never `rc.local`: a service gets a unit with an `[Install]` section (`WantedBy=multi-user.target`), a kernel module a file in `/etc/modules-load.d/`, a sysctl a file in `/etc/sysctl.d/`, a setting with no config file of its own a oneshot unit (`pve`'s `proos-boot.service`), and services that must start one after another with pauses between them stay disabled and are started by a oneshot unit (`appletalk`'s `afp-start.service`: wait 10 s, autofs, `modprobe appletalk`, atalkd, wait 10 s, netatalk and avahi, as its old `rc.local` did, since atalkd fails until the network has its address and netatalk joins AppleTalk only if atalkd is already up). In an LXC only `net.*` settings can be changed (the rest of `/proc/sys` is read-only), so each LXC keeps its settings in `config/sysctl-net.conf`, installed as `/etc/sysctl.d/90-net.conf` and applied at boot by systemd-sysctl: IPv6 off in every LXC, plus low ports for non-root users on `media` (Jellyfin) and `ping_group_range` on `mgmt`. Nothing else goes in it.
- Secrets are git-ignored and must stay out of commits and docs: `mgmt/keys/`, `*.rsa`, `*.pub`, `*.pem`, `*.crt`, `*.key`, `authorized_keys`, `auto.creds`, `creds.h`, `sasl_passwd`, `pia-wg.conf`. Don't print their contents.
- `._*` and `.DS_Store` files are macOS metadata, ignored by git; leave them alone.
- Large binaries in the tree (the guest backups `<host>/vzdump-*` and `pve/vmbkps/`, `rpi/effects/videos/`, `shared/fontawesome/`, firmware builds, photos) are backups or assets, not sources to read. Photos of the builds are in `btpi/photos`, `ktchpi/photos`, `ledwall/photos` and `pve/photos`, and are not deployed.
- A repo script is run through `bash` (`bash login.sh`, `bash www-minify.sh`), not by its execute bit, which mgmt sees as stored on the server: a file rewritten over the SMB share comes back `0644`, and this workstation's mount shows every file as `0755` whatever it is. A script a program must execute itself (ssh-add's `SSH_ASKPASS`) gets a wrapper written outside the repo; the Pi and server deploys `chmod +x` their installers.
- There is no git or node in this workstation's shell, and no test suite: verify shell changes by reading them, and the website as described in the Automate reference.

Code style: [CODESTYLE.md](CODESTYLE.md)

@CODESTYLE.md

## Automate website reference

Automate reference: [shared/CLAUDE.md](shared/CLAUDE.md)

@shared/CLAUDE.md
