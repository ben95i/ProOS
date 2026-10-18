## Code style

These are the code style rules for everything in ProOS. Project structure (where files and features go) is described in [`README.md`](README.md).

### General

- Use descriptive names: camelCase for variables and functions, UPPERCASE for constants. In shell/bash only, function names are UPPERCASE as well.
- Do not write inline expressions and do not simplify expressions; prefer the more verbose form.
- Break mathematical operations down into clearly named intermediate variables, so each step is readable and explainable.
- Consolidate similar code into common functions to reduce duplication. Follow DRY (Don't Repeat Yourself) and SOLID principles where that is possible without changing functionality.
- Add only the code a feature needs, leave existing features as they are, and reuse the project's existing helpers rather than writing new ones.
- Detect and fix any race conditions or memory management issues.
- Test that the code behaves the same before and after a modification.

### Comments

- Give every function a comment describing what it does and how to use it, with a short data example where it helps.
- Add why only when the reason is not obvious from the code.
- Keep comments concise, and keep them describing what the code currently does.

### Shell scripts

- Bash, with `## Comment` section headers.
- Install files one at a time: copy, then `chmod` / `chown` each installed file.
- Installers must be re-runnable: guard one-time steps with a marker file (`/etc/rpi-conf.done`, `/etc/iommu.enabled`, `/etc/zfsautosnap.enabled`), and install packages with `apt-get install --no-upgrade --ignore-missing`.
- Older scripts still use ALL_CAPS variables; convert them to camelCase when a script is reworked.

### JavaScript (Automate website)

- Draw a grid's or a window's buttons with `gridButtonTemplate()` and `dropdownButtonTemplate()` rather than writing the `<button>` markup out.
- Give every button an id; handlers pick a button by its id, not its place in the window.
- Register a template's buttons with `registerListeners()` directly under the template.
- Reuse the site's own helpers: `sendCmd`, `openDeviceMenu`, `drawMenu`, `promptWindow`, `onButtons`, `readFile` / `writeFile`, and `reportError` / `showPopup` from `error.js` for failures and notices.

### CSS (Automate website)

- No `+` inside `calc()`: yui-compressor strips the spaces around it. Write sums as `a - -1 * b`.
- Wrap every `:hover` rule in `@media (hover: hover) and (pointer: fine)`, or iOS keeps the last tapped row lit.
- Put translucency in the background colour, not `opacity`, on menus; iOS Safari repaints opacity layers poorly.
- The palette lives once on `:root`, never on `*`.
