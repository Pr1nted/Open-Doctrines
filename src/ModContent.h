#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Content a mod adds to the game's catalogues.
 *
 * ── ONE REGISTRY, NOT FIVE APIS ──
 *
 * Doctrines, research nodes, troop types, artillery types and district laws
 * are all the same shape of thing: a catalogue loaded once from JSON and read
 * everywhere by id. Giving each its own add/remove/list would be five APIs to
 * keep in step, five sets of persistence rules, and a sixth to write the next
 * time the game grows a catalogue.
 *
 * So there is one, and the definition crosses as the SAME JSON the data file
 * uses. A mod-added doctrine goes through exactly the parser data/policies.json
 * goes through -- not a second interpretation of the same fields, which is how
 * "it works from the file but not from the mod" bugs are made.
 *
 * ── PERSIST OR HOLLOW, AS WITH COUNTRY FIELDS ──
 *
 * PERSIST writes the definition into the save. A world that was played with a
 * mod's doctrine keeps knowing what that doctrine was, so a save does not
 * become unreadable the moment the mod is uninstalled -- the country still
 * holds "com.example.mod:land_reform" and the save can still say what it did.
 *
 * HOLLOW is redeclared on every load. Right for content a mod generates from
 * something else, and for a mod that would rather ship one definition than
 * have old ones fossilise in saves.
 *
 * ── AI VISIBILITY IS THE MOD'S CHOICE ──
 *
 * A doctrine the AI cannot see is one only a player is offered. That is a real
 * thing to want -- a flavour doctrine, a scripted event's payload, something
 * balanced for a human -- and it is also the honest default for anything the
 * AI was never trained against.
 *
 * It matters more than it looks for RESEARCH. The research tree feeds the
 * neural feature vector, so a mod adding a node the AI can see changes the
 * shape of the model's input, and a model whose parent does not match is
 * silently re-initialised (see the parent-must-match-the-code note in the AI
 * journal). Invisible content cannot do that, which is why it is the default.
 */
namespace odcontent {

/** Which catalogue. Values are ABI and only ever appended. */
enum class Kind : uint8_t {
    Doctrine = 0,
    Research = 1,
    TroopType = 2,
    Artillery = 3,
    DistrictLaw = 4,
    Count_ = 5
};

/** Whether the definition is written into the save. */
enum class Mode : uint8_t { Hollow = 0, Persist = 1 };

const char* kindName(Kind k);
/** The kind for a name, or Count_ if there is none. */
Kind kindFromName(const std::string& name);

struct Entry {
    Kind kind = Kind::Doctrine;
    std::string modId;
    std::string id;         ///< the catalogue id, as the game will know it
    std::string json;       ///< the definition, in the data file's own shape
    Mode mode = Mode::Hollow;
    /**
     * Whether the AI may consider this at all.
     *
     * False by default: content the AI was never trained against should not
     * start appearing in its options, and for research it would change the
     * model's input shape. A mod says `"aiVisible": true` in the definition to
     * opt in.
     */
    bool aiVisible = false;
    /** False for an entry read back from a save whose mod is gone. */
    bool ownerPresent = true;
};

class Registry {
public:
    /**
     * Add or replace one entry. False for a bad id, an unparseable kind, or an
     * id another mod already owns -- catalogue ids are global, because a
     * country holds a doctrine BY ID and two meanings for one id would make a
     * save ambiguous.
     */
    bool add(Kind kind, const std::string& modId, const std::string& id,
             const std::string& json, Mode mode, bool aiVisible);

    /** Remove one of your own. False if it was not yours. */
    bool remove(Kind kind, const std::string& modId, const std::string& id);

    /** Which mod owns this id in this catalogue, or empty. */
    std::string ownerOf(Kind kind, const std::string& id) const;

    /** Every entry of a kind, sorted by id. Includes ownerless ones. */
    std::vector<Entry> ofKind(Kind kind) const;
    /** Every entry a mod owns, sorted by kind then id. */
    std::vector<Entry> ofMod(const std::string& modId) const;
    /** How many entries a mod owns of a kind. */
    size_t countOf(Kind kind, const std::string& modId) const;

    void removeAllOf(const std::string& modId);
    void clear();

    /** Mark which mods are loaded; the rest go inert but keep their entries. */
    void setLoadedMods(const std::vector<std::string>& modIds);
    /** Drop every hollow entry. The mod redeclares them on load. */
    void clearHollow();

    /** Whether this mod has put anything in the save. */
    bool persistsAnything(const std::string& modId) const;

    /** Only the persisted entries, for the save. Sorted. */
    std::vector<Entry> toSave() const;
    /** Replace the persisted entries. Hollow ones are untouched. */
    void fromSave(const std::vector<Entry>& in);

private:
    std::vector<Entry> m_entries;
};

/**
 * Whether a catalogue id is usable.
 *
 * Stricter than a field name, because this id is written into saves, compared
 * against data-file ids, and may appear in a script: lower-case letters,
 * digits, underscore and a single colon for a mod's own namespace.
 */
bool validContentId(const std::string& id);

}  // namespace odcontent
