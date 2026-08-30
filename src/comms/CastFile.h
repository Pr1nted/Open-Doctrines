#pragma once
// One definition of what data/comms/cast.json means.
//
// WHY THIS EXISTS. The game reads the cast file, and so does
// TransmissionPreview, which is the tool the window is actually tuned in.
// When each of them mapped the JSON onto a Profile in its own code, the two
// drifted -- the preview kept a hand-written copy of Mia with the OLD lid
// settings, so a change made in cast.json was invisible in the very tool used
// to look at it, and the render it produced was a picture of a character that
// no longer existed. An hour went into the resulting "bug".
//
// So the mapping lives here, once, and anything that loads a speaker calls it.
#include "comms/Transmission.h"
#include "json.hpp"

#include <string>

namespace comms {

/// Fill `p` from one character's object in cast.json.
/// `imageDir` is prefixed to "image" (the game's data dir, "data/comms/" for tools).
inline void profileFromJson(const nlohmann::json& c, const std::string& imageDir, Profile& p) {
    if (c.contains("image")) p.image = imageDir + c["image"].get<std::string>();
    p.imgEyeX    = c.value("eye_x", p.imgEyeX);
    p.imgEyeY    = c.value("eye_y", p.imgEyeY);
    p.imgEyeGap  = c.value("eye_gap", p.imgEyeGap);
    p.imgEyeW    = c.value("eye_w", p.imgEyeW);
    p.imgEyeH    = c.value("eye_h", p.imgEyeH);
    p.imgOutline = c.value("outline", p.imgOutline);
    p.eyeOpenArc = c.value("open_arc", p.eyeOpenArc);
    p.headW      = c.value("head_w", p.headW);
    p.headH      = c.value("head_h", p.headH);
    p.shoulderW  = c.value("shoulder_w", p.shoulderW);
    p.neckW      = c.value("neck_w", p.neckW);
    p.eyeGap     = c.value("eye_gap", p.eyeGap);
    p.eyeW       = c.value("eye_w", p.eyeW);
    p.eyeH       = c.value("eye_h", p.eyeH);
    p.hat        = c.value("hat", p.hat);
    p.eyes       = c.value("eyes", p.eyes);
    p.lidBottom  = c.value("lid_bottom", p.lidBottom);
    p.lidRest    = c.value("lid_rest", p.lidRest);
    p.lashes     = c.value("lashes", p.lashes);
    p.pupilAspect = c.value("pupil_aspect", p.pupilAspect);
    p.pupilScale = c.value("pupil_scale", p.pupilScale);
    p.lashLift   = c.value("lash_lift", p.lashLift);
    p.lashFlick  = c.value("lash_flick", p.lashFlick);
    p.lowerLid   = c.value("lower_lid", p.lowerLid);
    p.eyeArt     = c.value("eye_art", p.eyeArt);
    p.pupilRadius = c.value("pupil_radius", p.pupilRadius);
    p.mouth      = c.value("mouth", p.mouth);
    p.mouthX     = c.value("mouth_x", p.mouthX);
    p.mouthY     = c.value("mouth_y", p.mouthY);
    p.mouthW     = c.value("mouth_w", p.mouthW);
    p.mouthBase  = c.value("mouth_base", p.mouthBase);
    p.hueShift   = c.value("hue_shift", p.hueShift);
    p.tintGain   = c.value("tint_gain", p.tintGain);
    p.hatBrim    = c.value("hat_brim", p.hatBrim);
    p.browDrop   = c.value("brow", p.browDrop);
}

/// The per-speaker seed: derived from the name, so a speaker's blink rhythm
/// and gaze are its own and are the same on every machine.
inline unsigned seedFromName(const std::string& name) {
    unsigned s = 1;
    for (unsigned char ch : name) s = s * 131u + ch;
    return s;
}

}  // namespace comms
