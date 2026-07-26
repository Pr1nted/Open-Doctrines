/* Hello Panel, in Java — the same mod as sdk/examples/hello-panel/mod.c.
 *
 * ModExamplesTest renders this next to the other SDKs and fails the build if a
 * single string differs, so the formatting below is deliberately exact rather
 * than idiomatic.
 *
 * Two Java habits to avoid, both explained in sdk/java/README.md:
 *   - no String.getBytes(): it traps on TeaVM's WASI target.
 *   - no "a" + b concatenation: javac 9+ compiles that to an invokedynamic
 *     call into StringConcatFactory, which TeaVM does not implement. Build
 *     strings with an explicit StringBuilder, as below.
 */

package org.opendoctrines.example;

import org.opendoctrines.gearbox.Gearbox;
import org.teavm.interop.Export;

public final class HelloPanel {
    private static int panel;
    private static int cursor;          // 0-based, as the ABI is

    @Export(name = "mod_load")
    public static int modLoad() {
        if (Gearbox.env().isHeadless) {
            // A training run has no renderer, so registering a panel would be
            // a no-op. Skipping it makes the intent explicit.
            Gearbox.log(Gearbox.LOG_INFO, "hello-panel: headless, no UI");
            return 0;
        }

        panel = Gearbox.panelRegister("Hello Panel", 280, 150);
        if (panel == 0) {
            // UI was declared but revoked, or we hit the panel limit. Degrade
            // rather than trap.
            Gearbox.log(Gearbox.LOG_WARN, "hello-panel: no panel, running quiet");
        }
        return 0;               // non-zero would refuse the load
    }

    @Export(name = "mod_unload")
    public static void modUnload() {
        cursor = 0;
    }

    @Export(name = "mod_draw_panel")
    public static void modDrawPanel(int p, int w, int h) {
        Gearbox.drawRect(p, 0, 0, w, 1, 0x3C3C5AFF);

        Gearbox.drawText(p, 8, 8, 0xFFFFFFFF,
                new StringBuilder().append("Turn ")
                        .append(Gearbox.turnNumber()).toString());

        int count = Gearbox.countryCount();
        Gearbox.drawText(p, 8, 28, 0xB4B4C8FF,
                new StringBuilder().append("Countries: ").append(count).toString());

        if (count == 0) {
            Gearbox.drawText(p, 8, 52, 0x9696A0FF, "No world loaded");
            return;
        }
        if (cursor >= count) cursor = 0;

        int c = Gearbox.countryAt(cursor);
        if (c != Gearbox.INVALID) {
            Gearbox.drawText(p, 8, 52, 0xFFFFFFFF, Gearbox.countryName(c));

            Gearbox.drawText(p, 8, 72, 0xB4B4C8FF,
                    new StringBuilder().append("Provinces: ")
                            .append(Gearbox.countryProvinceCount(c)).toString());

            // (long) truncates toward zero, matching the C reference's
            // (uint64_t) cast: -50.25 prints as -50, not -51.
            Gearbox.drawText(p, 8, 92, 0xB4B4C8FF,
                    new StringBuilder().append("Treasury: ")
                            .append((long) Gearbox.countryTreasury(c)).toString());
        }

        if (Gearbox.button(p, 8, 116, 120, 24, "Next country")) {
            cursor++;
        }
    }

    /** TeaVM needs an entry point. A Gearbox mod is driven entirely through
     *  its exports, so there is nothing for it to do. */
    public static void main(String[] args) { }
}
