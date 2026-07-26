/* Gearbox.java — the OpenDoctrines mod ABI for Java (and any JVM language).
 *
 * Compiled to wasm by TeaVM's WEBASSEMBLY_WASI backend. See sdk/java/README.md
 * for why that target and not WEBASSEMBLY_GC.
 *
 * Two layers:
 *
 *   - GearboxRaw, one native per host import, taking pointers as int. That file
 *     is GENERATED from sdk/abi.json by tools/gen_bindings.py -- adding a
 *     function to the ABI does not mean editing anything here.
 *   - this class, which takes and returns Java types and does the UTF-8
 *     conversion and two-call sizing for you. Hand-written, deliberately.
 *
 * Strings need care here. TeaVM's WASI target has no working charset support --
 * String.getBytes() compiles but traps at runtime -- so this class carries its
 * own UTF-8 encoder and decoder. Do not reach for getBytes() in a mod; see the
 * README section "What does not work".
 *
 * The other direction -- the exports the host calls -- is not declared here,
 * because TeaVM takes them from annotations on your own class:
 *
 *     @Export(name = "mod_load")       static int  modLoad()
 *     @Export(name = "mod_unload")     static void modUnload()
 *     @Export(name = "mod_draw_panel") static void modDrawPanel(int p, int w, int h)
 *     @Export(name = "mod_pre_turn")   static void modPreTurn(int turn)
 *     @Export(name = "mod_post_turn")  static void modPostTurn(int turn)
 *
 * Only mod_load is mandatory, and the method must be static.
 */

package org.opendoctrines.gearbox;

import org.teavm.interop.Address;

public final class Gearbox {
    private Gearbox() { }

    public static final int LOG_TRACE = 0;
    public static final int LOG_INFO  = 1;
    public static final int LOG_WARN  = 2;
    public static final int LOG_ERROR = 3;

    /** Returned by countryAt/provinceOwner when there is no such entity. */
    public static final int INVALID = 0xFFFFFFFF;

    /* ---------------------------------------------------------------- raw --
     * The imports themselves are GENERATED into GearboxRaw from sdk/abi.json;
     * see tools/gen_bindings.py. They are not repeated here, so adding a
     * function to the ABI does not mean editing this file.
     *
     * Everything below is the ergonomic layer: UTF-8, two-call sizing, and
     * turning INVALID into something a Java caller expects. That part is
     * deliberately hand-written -- it is what makes this read as Java rather
     * than as a generated stub.
     *
     * GearboxRaw is public, so a mod doing something unusual can call the wire
     * ABI directly without forking this class.
     */

    /* ------------------------------------------------------------- utf-8 --
     * Hand-rolled because TeaVM's WASI target traps in String.getBytes().
     * Surrogate pairs are combined; an unpaired surrogate becomes U+FFFD
     * rather than producing invalid UTF-8 for the host to choke on.
     */

    static byte[] toUtf8(String s) {
        int n = 0;
        for (int i = 0; i < s.length(); i++) {
            int c = s.charAt(i);
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length()
                    && s.charAt(i + 1) >= 0xDC00 && s.charAt(i + 1) <= 0xDFFF) {
                n += 4; i++;
            } else if (c < 0x80)    n += 1;
            else if (c < 0x800)     n += 2;
            else                    n += 3;
        }
        byte[] out = new byte[n];
        int k = 0;
        for (int i = 0; i < s.length(); i++) {
            int c = s.charAt(i);
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length()
                    && s.charAt(i + 1) >= 0xDC00 && s.charAt(i + 1) <= 0xDFFF) {
                int cp = 0x10000 + ((c - 0xD800) << 10) + (s.charAt(i + 1) - 0xDC00);
                out[k++] = (byte) (0xF0 | (cp >> 18));
                out[k++] = (byte) (0x80 | ((cp >> 12) & 0x3F));
                out[k++] = (byte) (0x80 | ((cp >> 6) & 0x3F));
                out[k++] = (byte) (0x80 | (cp & 0x3F));
                i++;
            } else if (c >= 0xD800 && c <= 0xDFFF) {
                out[k++] = (byte) 0xEF; out[k++] = (byte) 0xBF; out[k++] = (byte) 0xBD;
            } else if (c < 0x80) {
                out[k++] = (byte) c;
            } else if (c < 0x800) {
                out[k++] = (byte) (0xC0 | (c >> 6));
                out[k++] = (byte) (0x80 | (c & 0x3F));
            } else {
                out[k++] = (byte) (0xE0 | (c >> 12));
                out[k++] = (byte) (0x80 | ((c >> 6) & 0x3F));
                out[k++] = (byte) (0x80 | (c & 0x3F));
            }
        }
        return out;
    }

    /** Decodes `len` bytes of UTF-8. Malformed input yields U+FFFD, never an
     *  exception -- a mod should not die because a name was odd. */
    static String fromUtf8(byte[] b, int len) {
        StringBuilder sb = new StringBuilder(len);
        int i = 0;
        while (i < len) {
            int c = b[i] & 0xFF;
            int cp; int extra;
            if (c < 0x80)        { cp = c;        extra = 0; }
            else if (c < 0xC0)   { sb.append('�'); i++; continue; }
            else if (c < 0xE0)   { cp = c & 0x1F;  extra = 1; }
            else if (c < 0xF0)   { cp = c & 0x0F;  extra = 2; }
            else if (c < 0xF8)   { cp = c & 0x07;  extra = 3; }
            else                 { sb.append('�'); i++; continue; }

            if (i + extra >= len) { sb.append('�'); break; }
            for (int k = 1; k <= extra; k++) {
                int cc = b[i + k] & 0xFF;
                if ((cc & 0xC0) != 0x80) { cp = -1; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            i += extra + 1;
            if (cp < 0)              sb.append('�');
            else if (cp > 0x10FFFF)  sb.append('�');
            else if (cp >= 0x10000) {
                cp -= 0x10000;
                sb.append((char) (0xD800 + (cp >> 10)));
                sb.append((char) (0xDC00 + (cp & 0x3FF)));
            } else sb.append((char) cp);
        }
        return sb.toString();
    }

    private static int ptr(byte[] b) { return Address.ofData(b).toInt(); }

    /* ---------------------------------------------------------------- core -- */

    public static void log(int level, String msg) {
        byte[] b = toUtf8(msg);
        GearboxRaw.log(level, ptr(b), b.length);
    }

    public static void abort(String msg) {
        byte[] b = toUtf8(msg);
        GearboxRaw.abort(ptr(b), b.length);
    }

    /** The budget for the current hook, or Long.MAX_VALUE when unmetered.
     *  This is the LIMIT, not a countdown -- it does not fall as you run. */
    public static long fuelBudget() {
        long f = GearboxRaw.fuelBudget();
        return f == -1L ? Long.MAX_VALUE : f;   // UINT64_MAX arrives as -1
    }

    /** Host environment. Field order matches gearbox_env_t; `size` is written
     *  before the call so an older mod stays safe against a newer host. */
    public static final class Env {
        public int gearboxMajor, gearboxMinor, hostVersion, platform;
        public boolean isWeb, isHeadless;
        public int screenW, screenH;
    }

    private static final int ENV_SIZE = 28;

    public static Env env() {
        byte[] buf = new byte[ENV_SIZE];
        Address a = Address.ofData(buf);
        a.putInt(ENV_SIZE);                     // out->size, per the ABI
        GearboxRaw.env(a.toInt());

        Env e = new Env();
        e.gearboxMajor = a.add(4).getInt();
        e.gearboxMinor = a.add(8).getInt();
        e.hostVersion  = a.add(12).getInt();
        e.platform     = buf[16] & 0xFF;
        e.isWeb        = buf[17] != 0;
        e.isHeadless   = buf[18] != 0;
        e.screenW      = a.add(20).getInt();
        e.screenH      = a.add(24).getInt();
        return e;
    }

    /* ------------------------------------------------------- gamestate.read -- */

    public static int turnNumber()   { return GearboxRaw.turnNumber(); }
    public static int countryCount() { return GearboxRaw.countryCount(); }

    /** 0-based, as the ABI is. Returns INVALID when out of range. */
    public static int countryAt(int index) { return GearboxRaw.countryAt(index); }

    public static double countryTreasury(int country) { return GearboxRaw.countryTreasury(country); }
    public static int countryProvinceCount(int country) { return GearboxRaw.countryProvinceCount(country); }
    public static long provincePopulation(int province) { return GearboxRaw.provincePopulation(province); }
    public static int provinceOwner(int province) { return GearboxRaw.provinceOwner(province); }

    /** Does the ABI's two-call sizing and hands back a finished String. */
    public static String countryName(int country) {
        int need = GearboxRaw.countryName(country, 0, 0);
        if (need <= 0) return "";
        byte[] buf = new byte[need];
        int got = GearboxRaw.countryName(country, ptr(buf), need);
        if (got > need) got = need;             // grew between the two calls
        return fromUtf8(buf, got);
    }

    /* ------------------------------------------------------------------ ui -- */

    public static int panelRegister(String title, int minW, int minH) {
        byte[] b = toUtf8(title);
        return GearboxRaw.panelRegister(ptr(b), b.length, minW, minH);
    }

    public static void drawText(int panel, int x, int y, int rgba, String text) {
        byte[] b = toUtf8(text);
        GearboxRaw.drawText(panel, x, y, rgba, ptr(b), b.length);
    }

    public static void drawRect(int panel, int x, int y, int w, int h, int rgba) {
        GearboxRaw.drawRect(panel, x, y, w, h, rgba);
    }

    /** True on the frame the button is released inside its bounds. */
    public static boolean button(int panel, int x, int y, int w, int h, String label) {
        byte[] b = toUtf8(label);
        return GearboxRaw.button(panel, x, y, w, h, ptr(b), b.length) != 0;
    }

    /* -------------------------------------------------------------- assets -- */

    public static int assetSize(String name) {
        byte[] n = toUtf8(name);
        return GearboxRaw.size(ptr(n), n.length);
    }

    /** The asset's bytes, or null if there is no such asset. */
    public static byte[] assetRead(String name) {
        byte[] n = toUtf8(name);
        int size = GearboxRaw.size(ptr(n), n.length);
        if (size <= 0) return null;
        byte[] buf = new byte[size];
        int got = GearboxRaw.read(ptr(n), n.length, ptr(buf), size);
        if (got >= size) return buf;
        byte[] out = new byte[got];
        for (int i = 0; i < got; i++) out[i] = buf[i];
        return out;
    }
}
