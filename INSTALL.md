# Installing NTSCRT

| App | Supported | Download |
|---|---|---|
| DaVinci Resolve (Windows) | 21.1.2 and later, x64 | `NTSCRT-OFX-windows-x64.zip` |
| DaVinci Resolve (macOS) | 21.1.2 and later, Apple silicon + Intel | `NTSCRT-OFX-macos-universal.zip` |
| Final Cut Pro (macOS) | 12.4 and later, Apple silicon + Intel | `NTSCRT-FxPlug-macos-universal.zip` |

**Where to get the zips:** the latest release on the repo's **Releases** page. Until releases are published,
use the build artifacts: **Actions → ci → latest green run on `main` → Artifacts** (GitHub wraps each
artifact in one more zip; unzip it to get the file above).

---

## DaVinci Resolve — Windows (about 1 minute)

1. **Quit Resolve.**
2. Unzip `NTSCRT-OFX-windows-x64.zip` (right-click → *Extract All*). Don't run anything from inside the zip.
3. In the extracted folder, double-click **`Install NTSCRT.cmd`** and click **Yes** at the Windows admin prompt.
4. Start Resolve. The effect is in **Effects → OpenFX → Filters → NTSCRT**. Drag it onto a clip, or into a node
   on the Color page.

<details><summary>Manual install / uninstall</summary>

- Manual: copy the `NTSCRT.ofx.bundle` folder into `C:\Program Files\Common Files\OFX\Plugins\`
  (create `OFX\Plugins` if it doesn't exist), then restart Resolve.
- Uninstall: run `powershell -ExecutionPolicy Bypass -File install-windows.ps1 -Uninstall` from the
  extracted folder, or delete `C:\Program Files\Common Files\OFX\Plugins\NTSCRT.ofx.bundle`.
</details>

## DaVinci Resolve — macOS (about 1 minute)

1. **Quit Resolve.**
2. Double-click `NTSCRT-OFX-macos-universal.zip` to unzip it.
3. Open **Terminal**, type `sh ` (with a trailing space), drag **`install-macos.sh`** from the unzipped folder into
   the window, press **Return**, and enter your Mac password when asked.
   The script copies the plugin to `/Library/OFX/Plugins/` and clears the download quarantine so macOS lets Resolve load it.
4. Start Resolve. The effect is in **Effects → OpenFX → Filters → NTSCRT**.

<details><summary>Manual install / uninstall</summary>

- Manual: copy `NTSCRT.ofx.bundle` into `/Library/OFX/Plugins/`, then in Terminal run
  `sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/NTSCRT.ofx.bundle` and restart Resolve.
- Uninstall: `sh install-macos.sh --uninstall`, or delete `/Library/OFX/Plugins/NTSCRT.ofx.bundle`.
</details>

## Final Cut Pro — macOS (about 2 minutes)

1. **Quit Final Cut Pro.**
2. Unzip `NTSCRT-FxPlug-macos-universal.zip` and drag **`NTSCRT.app`** into **Applications**.
3. Open **NTSCRT.app** once. It registers the plug-in and installs the effect template, then quits.
   - If macOS says the app "can't be opened" (current builds aren't notarized): open **System Settings →
     Privacy & Security**, scroll down, click **Open Anyway** next to NTSCRT, and open the app again.
     (Or in Terminal: `xattr -dr com.apple.quarantine /Applications/NTSCRT.app`.)
4. Start Final Cut Pro. The effect is in the **Effects browser → NTSCRT**. Drag it onto a clip, then adjust
   it in the **Video Inspector**.

**Keep `NTSCRT.app` in Applications.** The plug-in lives inside it, so deleting or moving the app removes the effect.

<details><summary>Uninstall</summary>

Delete `/Applications/NTSCRT.app` and the folder
`~/Movies/Motion Templates.localized/Effects.localized/NTSCRT`.
</details>

---

## Using it

- **Preset** menu: pick a look (Clean VHS, Medium VHS, V Hold glitch, Webcam…). It sets every control, and you can
  then tweak and keyframe any of them.
- **Screen Loop preset**: video feedback (a camcorder filming its own TV).
- **Load preset… / Save preset…**: preset files are the same JSON as the NTSCRT app, and the NTSC section is
  compatible with ntsc-rs presets.
- Apply it to the clip at its **native resolution**. On a low-res clip in a high-res timeline, the analog artifacts get
  scaled up with the picture.
- Screen Loop and the Digital-video codec depend on earlier frames, so the first render after jumping far
  into a clip can take a moment.

## Troubleshooting

| Problem | Fix |
|---|---|
| Effect doesn't show up in Resolve | Restart Resolve fully. Check that the bundle is in the OFX folder above and that its whole folder was copied (`Contents\Win64\NTSCRT.ofx` on Windows, `Contents/MacOS/NTSCRT.ofx` on macOS). |
| Resolve skips the plugin at startup | Resolve marks plugins that crashed as disabled. Re-enable it in *Preferences → Video Plugins* and send us the log. |
| macOS: "NTSCRT.ofx is damaged / can't be verified" | Run the install script again: it clears the quarantine. For a manual copy, run the `xattr` command above. |
| Final Cut: effect missing | Open `NTSCRT.app` once more and restart Final Cut. In Terminal, `pluginkit -mAvvv -p FxPlug` should list `NTSCRT Renderer`. |
| Picture is black or unchanged | Usually a partial copy: the `shaders/` folder or `librashader` file inside the bundle is missing. Reinstall from the zip. Resolve shows the plugin's error message when it can't render. |

Building from source instead: see `README.md` (Windows/Resolve) and `fxplug/README.md` (Final Cut).
