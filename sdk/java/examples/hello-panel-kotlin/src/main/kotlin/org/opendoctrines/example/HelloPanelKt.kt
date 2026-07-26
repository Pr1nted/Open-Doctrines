/* Hello Panel, in Kotlin — the same mod again, on the same binding.
 *
 * Kotlin needs no separate Gearbox binding: TeaVM consumes JVM bytecode, so it
 * compiles kotlinc's output exactly as it compiles javac's, and this file calls
 * the same org.opendoctrines.gearbox.Gearbox as the Java example.
 *
 * Two Kotlin-specific things worth knowing, both in sdk/java/README.md:
 *   - @Export must land on a genuinely static method, hence @JvmStatic in an
 *     `object`. On a plain class method the export silently will not appear.
 *   - No string templates ("Turn $n"). kotlinc compiles those to an
 *     invokedynamic against StringConcatFactory on modern JVM targets, which
 *     TeaVM does not implement. Build strings with StringBuilder, as below.
 */

package org.opendoctrines.example

import org.opendoctrines.gearbox.Gearbox
import org.teavm.interop.Export

object HelloPanelKt {
    private var panel = 0
    private var cursor = 0      // 0-based, as the ABI is

    @JvmStatic
    @Export(name = "mod_load")
    fun modLoad(): Int {
        if (Gearbox.env().isHeadless) {
            Gearbox.log(Gearbox.LOG_INFO, "hello-panel: headless, no UI")
            return 0
        }
        panel = Gearbox.panelRegister("Hello Panel", 280, 150)
        if (panel == 0) {
            Gearbox.log(Gearbox.LOG_WARN, "hello-panel: no panel, running quiet")
        }
        return 0
    }

    @JvmStatic
    @Export(name = "mod_unload")
    fun modUnload() {
        cursor = 0
    }

    @JvmStatic
    @Export(name = "mod_draw_panel")
    fun modDrawPanel(p: Int, w: Int, h: Int) {
        Gearbox.drawRect(p, 0, 0, w, 1, 0x3C3C5AFF.toInt())

        Gearbox.drawText(p, 8, 8, 0xFFFFFFFF.toInt(),
            StringBuilder().append("Turn ").append(Gearbox.turnNumber()).toString())

        val count = Gearbox.countryCount()
        Gearbox.drawText(p, 8, 28, 0xB4B4C8FF.toInt(),
            StringBuilder().append("Countries: ").append(count).toString())

        if (count == 0) {
            Gearbox.drawText(p, 8, 52, 0x9696A0FF.toInt(), "No world loaded")
            return
        }
        if (cursor >= count) cursor = 0

        val c = Gearbox.countryAt(cursor)
        if (c != Gearbox.INVALID) {
            Gearbox.drawText(p, 8, 52, 0xFFFFFFFF.toInt(), Gearbox.countryName(c))

            Gearbox.drawText(p, 8, 72, 0xB4B4C8FF.toInt(),
                StringBuilder().append("Provinces: ")
                    .append(Gearbox.countryProvinceCount(c)).toString())

            // toLong() truncates toward zero, matching the C reference.
            Gearbox.drawText(p, 8, 92, 0xB4B4C8FF.toInt(),
                StringBuilder().append("Treasury: ")
                    .append(Gearbox.countryTreasury(c).toLong()).toString())
        }

        if (Gearbox.button(p, 8, 116, 120, 24, "Next country")) {
            cursor++
        }
    }

    @JvmStatic
    fun main(args: Array<String>) { }
}
