#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

/**
 * Fields a mod adds to a country.
 *
 * ── WHY THE GAME CANNOT JUST HAVE MORE FIELDS ──
 *
 * A mod that invents a mechanic needs somewhere to keep it. Storage gets it a
 * private key-value blob, but that is a per-mod FILE beside the save: it does
 * not branch when a save branches, it does not travel in multiplayer, and
 * loading a campaign from three turns ago leaves the mod's numbers at their
 * newest values. Anything shaped like "this country has a morale of 62" wants
 * to live where the country lives.
 *
 * So a mod declares a field, and every country gets one.
 *
 * ── PERSIST OR HOLLOW, AND THE MOD CHOOSES ──
 *
 * The two differ in exactly one way: whether the values go into the save.
 *
 *   PERSIST   written into the save and read back. The field survives a
 *             restart, branches with the save, and is the right choice for
 *             anything the player would be upset to lose.
 *
 *   HOLLOW    never written. The mod re-declares the field on load and fills
 *             it from whatever it can recompute. Right for a cache, a derived
 *             number, or anything a mod would rather recompute than trust.
 *
 * ── A PERSISTED FIELD SURVIVES ITS MOD BEING UNINSTALLED ──
 *
 * This is the part that makes removal reversible, and it is a deliberate
 * choice against the easier one. When a save is loaded without the mod that
 * owns a persisted field, the values are NOT dropped: they are held verbatim
 * and written back out on the next save. Uninstall the mod, play twenty turns,
 * reinstall it, and its numbers are still there.
 *
 * The alternative -- discard what has no owner -- is simpler and silently
 * destroys a player's data the first time they disable a mod to check whether
 * it was causing something. Holding unowned values costs a few bytes per
 * country and makes the decision undoable, so the game holds them.
 *
 * ── A FIELD BELONGS TO ONE MOD ──
 *
 * Keyed by (mod id, field name), so two mods may both add "morale" without
 * colliding and neither can read or write the other's. That is the same
 * isolation Storage has, and for the same reason: a mod is not entitled to
 * another mod's state just because it guessed the name.
 */
namespace odcountry {

/** Whether a field's values are written into the save. */
enum class Mode : uint8_t { Hollow = 0, Persist = 1 };

/** A value is a number or a string; a field is one or the other for its life. */
enum class Type : uint8_t { Number = 0, Text = 1 };

struct Field {
    std::string modId;
    std::string name;
    Mode mode = Mode::Hollow;
    Type type = Type::Number;
    /**
     * Whether the mod that owns this field is loaded right now.
     *
     * False for a field read back from a save whose mod is gone. Such a field
     * is inert -- nothing reads or writes it -- but its values are kept and
     * written back out, so reinstalling the mod restores them. See the note
     * above about removal being reversible.
     */
    bool ownerPresent = true;
};

/** Every field, and every country's value for it. */
class Store {
public:
    /**
     * Declare a field. Returns false if the name is unusable or the same mod
     * already declared it with a different type -- redeclaring identically is
     * fine and is what a hollow field does on every load.
     */
    bool add(const std::string& modId, const std::string& name, Mode mode, Type type);

    /** Forget a field and every value of it. Returns whether it existed. */
    bool remove(const std::string& modId, const std::string& name);

    bool has(const std::string& modId, const std::string& name) const;

    bool setNumber(const std::string& modId, const std::string& name,
                   int countryId, double v);
    double number(const std::string& modId, const std::string& name,
                  int countryId, double fallback = 0.0) const;

    bool setText(const std::string& modId, const std::string& name,
                 int countryId, const std::string& v);
    std::string text(const std::string& modId, const std::string& name,
                     int countryId) const;

    /** Every field a given mod owns, sorted by name. */
    std::vector<Field> fieldsOf(const std::string& modId) const;
    /** Every field, sorted by (mod, name). For the save and for diagnostics. */
    std::vector<Field> all() const;

    /**
     * Mark which mods are loaded.
     *
     * A field whose owner is absent keeps its values and stops answering: the
     * point is that reinstalling brings it back, not that a missing mod's data
     * quietly stays writable by nobody.
     */
    void setLoadedMods(const std::vector<std::string>& modIds);

    /** Drop every HOLLOW field. Called on load: the mod redeclares them. */
    void clearHollow();

    /** Everything, gone. */
    void clear();

    /** Whether any mod has declared a field that goes into the save. */
    bool anyPersisted() const;
    /** Whether this mod has. Fills WorldProvenance's `persisted` flag. */
    bool persistsAnything(const std::string& modId) const;

    // ── save shape ──
    struct Value {
        int countryId = 0;
        double number = 0.0;
        std::string text;
    };
    struct Saved {
        Field field;
        std::vector<Value> values;    ///< sorted by countryId
    };
    /** Only the PERSIST fields, which is all a save should carry. */
    std::vector<Saved> toSave() const;
    /** Replace the persisted fields with these. Hollow fields are untouched. */
    void fromSave(const std::vector<Saved>& in);

private:
    struct Entry {
        Field field;
        std::map<int, double> numbers;
        std::map<int, std::string> texts;
    };
    /** key is modId + '\0' + name, so neither can be confused with the other. */
    std::map<std::string, Entry> m_fields;
    static std::string key(const std::string& modId, const std::string& name);
    const Entry* find(const std::string& modId, const std::string& name) const;
    Entry* find(const std::string& modId, const std::string& name);
};

/** Whether a name is usable as a field name. See the definition for the rules. */
bool validFieldName(const std::string& name);

}  // namespace odcountry
