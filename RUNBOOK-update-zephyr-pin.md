# RUNBOOK — Moving the Zephyr pin forward (agent-driven)

**Audience: Claude Code.** Follow this when the user wants to update this workspace to a
newer Zephyr mainline commit — e.g. to pick up new PIC32CM PL10 support Microchip merged
upstream. It is an *interview*: ask for the target commit, apply the change, validate it
end-to-end, then persist the pin. Do **one** change per build iteration and never write a
pin you have not actually built and flashed.

Background on *why* the mechanism works this way (`west update` never touches `zephyr/`,
so the pin must be moved by hand): `RUNBOOK.md` → Appendix B. This runbook is the
executable checklist for doing it.

> Paths below use `C:\zw` as the example workspace root (as the rest of the docs do). Use
> the user's actual workspace path. The pinned values live at the top of
> `reproduce-install.ps1`: `$ZEPHYR_REV`, `$SDK_VER`, `$WEST_VER`, `$PROJECT_FILTER`.

---

## Preconditions — you need a *reproduced* workspace, not just a clone

The validation steps (1–6) run `git checkout` / `west update` / `west build` **inside**
`C:\zw\zephyr`, so they assume a **fully reproduced** workspace already exists on this
machine — i.e. `reproduce-install.ps1` has already been run, so `zephyr/`, `modules/`,
`.venv/` and the SDK are present. A bare `git clone` of this repo is **not** enough: the
To-Go repo ships only `app/`, the scripts, and docs — `zephyr/` does not exist until
reproduced, and there's nothing to `git checkout` in until it does.

Determine the entry point first:

- **A reproduced `C:\zw` already exists here** → normal path; continue to Step 0.
- **Only a fresh clone (no `zephyr/` yet)** → run `reproduce-install.ps1` **once with the
  current pin** to get a working workspace, *then* follow this runbook to move it forward.
  Do **not** shortcut by editing `$ZEPHYR_REV` and reproducing straight to the new commit —
  that pins a state before you've built and flashed it, exactly what Step 7's golden rule
  forbids.

Then activate the venv in your shell so `west`/`pyocd` are on `PATH`:
`& C:\zw\.venv\Scripts\Activate.ps1`.

---

## Step 0 — Interview the user (do not skip, do not guess)

Ask, and wait for answers, before touching anything:

1. **Which commit / tag?** Get the exact Zephyr mainline commit SHA or release tag to move
   to (e.g. a 40-char SHA, or `v4.2.0`). Do **not** invent one and do **not** default to
   `origin/main` unless the user explicitly asks to track latest — the whole point of this
   workspace is a pinned, tested state.
2. **Why this commit?** Ideally the user names the upstream PR/commit that added the PL10
   change. If they only say "the newest", note that "newest" is riskier (untested, may drag
   SDK/west bumps) and confirm they accept that.
3. **Which workspace to validate in?** You need a real build to trust the pin:
   - an **existing** `C:\zw` workspace (fast — reuses the clone), or
   - a **fresh** from-scratch reproduction (slow — ~1.8 GB, 5–20 min, but proves the
     `reproduce-install.ps1` path itself).
   Recommend the existing workspace for the build test, then persist the pin so the next
   fresh reproduction matches.

If the user cannot give a concrete commit, stop and say so — do not proceed with a placeholder.

## Step 1 — Sanity-check the commit exists and is what they think

In the existing `zephyr/` clone:

```powershell
Set-Location C:\zw\zephyr
git fetch origin
git cat-file -t <hash>          # must print "commit"; if "fatal: Not a valid object name", the fetch didn't bring it — stop
git log -1 --oneline <hash>     # show the user what they're about to pin to; confirm it's the intended commit
```

If the user gave a tag, resolve it: `git rev-parse <tag>^{commit}` → use that SHA as the
canonical pin value (tags can move; a SHA cannot).

Optional but valuable: confirm the PL10 change is actually *in* that commit's history, e.g.
`git log --oneline 454f925..<hash> -- boards drivers | Select-String -Pattern 'pl10|pic32cm' -CaseSensitive:$false`
(replace `454f925` with the current `$ZEPHYR_REV`). If nothing relevant shows up, warn the
user that this commit may not contain what they expect.

## Step 2 — Apply the checkout in the validation workspace

```powershell
Set-Location C:\zw\zephyr
git checkout <hash>             # detached HEAD, exactly like the initial setup — NOT 'git pull' on a branch
Set-Location C:\zw
west update --narrow --fetch-opt=--depth=1
```

`west update` now reads the **new** `zephyr/west.yml` and pulls any module revision bumps
that commit requires (e.g. a `hal_microchip` bump backing a new driver). Watch its output
for which modules changed.

## Step 3 — Fallstrick: does a needed module survive the project-filter?

The workspace clones only an allow-list of modules
(`$PROJECT_FILTER = -.*,+hal_microchip,+cmsis,+cmsis_6,+picolibc`). If the new Zephyr code
needs a module **not** on that list, `west update` silently skips it and the build fails
later with a missing header / missing Kconfig symbol.

- If the build (Step 4) fails on a missing `hal_X` / module header, widen the filter for
  that **one** project and re-update:
  ```powershell
  west config manifest.project-filter -- "-.*,+hal_microchip,+cmsis,+cmsis_6,+picolibc,+hal_X"
  west update --narrow --fetch-opt=--depth=1
  ```
- Then mirror the same string into `$PROJECT_FILTER` in `reproduce-install.ps1` (Step 7) so
  a fresh reproduction stays consistent.

## Step 4 — Pristine rebuild (one change, clean cache)

```powershell
Set-Location C:\zw\zephyr
$env:CMAKE_GENERATOR = "Ninja"
west build -p always -b pic32cm_pl10_cnano -d C:\zw\build C:\zw\app
```

`-p always` is mandatory after a pin move — a stale CMake cache would skew the result.

**Fallstrick: SDK / west compatibility.** If configure/build fails with "SDK version not
supported", a Kconfig/DTS schema mismatch, or a west manifest-schema error, the newer
Zephyr wants a newer toolchain or west than the current pins:
- SDK: `west sdk install -t arm-zephyr-eabi -H --version <newer>`, then bump `$SDK_VER`.
- west: `pip install "west==<newer>"`, then bump `$WEST_VER`.
Treat each as its own one-change-then-rebuild iteration.

## Step 5 — Fallstrick: memory headroom (this part has almost none)

After a green build, re-check — new Zephyr versions routinely grow RAM/flash:

```powershell
west build -d C:\zw\build -t ram_report
west build -d C:\zw\build -t rom_report
```

This board has **8 KB RAM / 60 KB flash** and the app already sits around 61% / 30%. If a
region overflowed or RAM jumped materially, that is a blocker — report the new numbers to
the user before continuing, and update the figures in `README.md` (Memory usage) and
`CLAUDE.md` if the pin is kept.

## Step 6 — Flash and exercise the board

```powershell
Set-Location C:\zw\build
west flash
```

If flashing stops with `SWD/JTAG communication failure (FAULT ACK)`: reset once and retry
(`pyocd reset -t pic32cm6408pl10048 -f 100000`, then `west flash`) — and keep `pyocd` at
**0.43.0** (0.44.1 is broken for this board). Then, over the 115200-baud console, verify
the app still works: `help`, `led blink 100`, `adc read`, `threads`, `reset`.

**Fallstrick: new interrupt sources.** If the new Zephyr revision pulled in a driver that
adds interrupt sources, the trimmed `CONFIG_ISR_STACK_SIZE` (1024) may no longer be safe.
Re-run the ISR stress test (fast blink + rapid concurrent `help`/`adc` over `pyserial` — see
`CLAUDE.md` / `RUNBOOK.md` Step 7) and watch `threads` high-water marks before trusting the
stack tuning.

## Step 7 — Persist the pin (only after Steps 4–6 are green)

Now, and only now, edit `reproduce-install.ps1` so a fresh reproduction matches what you
just validated:

- `$ZEPHYR_REV` → the new commit SHA (the resolved SHA from Step 1, not a tag).
- `$SDK_VER` / `$WEST_VER` → only if you had to bump them in Step 4.
- `$PROJECT_FILTER` → only if you had to widen it in Step 3.

Keep the inline comment on any changed line accurate (dates, "verified" notes). If you
touched RAM/flash figures, also update `README.md` (Memory usage) and the `CLAUDE.md`
usage line.

## Step 8 — Report and commit

Summarize to the user: old SHA → new SHA, which modules bumped, SDK/west/filter changes if
any, new RAM/flash numbers, and the interaction-test result. Commit only when the user asks
(branch first if on the default branch), e.g.:

```
Bump Zephyr pin <oldshort> -> <newshort> for <reason>

<what moved: modules, SDK/west, filter>. Verified: pristine build, flash,
console interaction, ram/rom_report (<new numbers>).
```

---

## Fallstricke at a glance

| Symptom / risk | Cause | Action |
|---|---|---|
| Editing the `.ps1` "does nothing" | `$ZEPHYR_REV` only drives a **fresh** from-scratch run; an existing `C:\zw` is unaffected | Move the pin in the live clone via `git checkout` + `west update` (Steps 2), *then* persist to the script (Step 7) |
| `git cat-file` says not a valid object | Commit not fetched / typo'd hash | `git fetch origin` first; re-confirm the SHA with the user |
| Missing header / Kconfig symbol at build | Needed module excluded by `project-filter` | Widen filter for that one project, `west update` again, mirror to `$PROJECT_FILTER` (Step 3) |
| "SDK version not supported" / schema mismatch | Newer Zephyr needs newer SDK or west | Bump `$SDK_VER` / `$WEST_VER`, install, rebuild (Step 4) |
| FLASH/RAM "region overflowed" or RAM spikes | Newer Zephyr grew the footprint on an 8 KB part | Blocker — report numbers, reconsider the bump; update docs if kept (Step 5) |
| Fault only under interrupt load after bump | New driver added interrupt sources; `ISR_STACK_SIZE` (1024) too low | Re-run the ISR stress test; raise if needed (Step 6) |
| `west flash` FAULT ACK | Sleeping core / leftover DAP / wrong pyOCD | `pyocd reset` then retry; keep `pyocd==0.43.0` (Step 6) |
| Reproducible build now differs per machine | Persisted a tag, not a SHA (tags move) | Pin the resolved commit SHA in `$ZEPHYR_REV`, never a tag |

**Golden rule:** never write a pin into `reproduce-install.ps1` that you have not built,
flashed, and exercised on the board in this same session.
