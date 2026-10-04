# Drawing this character

Four PNGs. Drop one in this folder and it appears on the body the next time the
game starts — no code change, no rig edit. Anything you have not drawn yet keeps
its procedural shape, so **you can draw one part, look at it on the real body in
a real pose, and decide whether the next one is worth drawing.**

## The four files

| file | what it is | canvas | pivot (cyan cross) | drawn |
|---|---|---|---|---|
| `head.png` | head **and hat**, one image | 534 × 442 | 50% across, 81% down | upright |
| `coat.png` | the coat body | 680 × 580 | 50% across, 62% down | upright |
| `armup.png` | one upper arm | 308 × 184 | 17% across, 50% down | pointing **right** |
| `armfore.png` | one forearm | 280 × 160 | 17% across, 50% down | pointing **right** |

The arms are used for **both** sides, so draw one of each.

`_template_*.png` in this folder are those canvases with the guides on them.
Open one, draw over it, delete the guide layer, save as the name in the table.
They are guides only and can be deleted.

## The three things that have to line up

**1. The pivot is the joint.** The cyan cross in the template is the point that
sits on the bone's joint, and the point the part rotates about. Everything
hangs off getting this right:

- `head.png` — the cross is where the head meets the **neck**, not the middle of
  the face. The head and hat are above it.
- `coat.png` — the cross is at the **hips**. The coat rises from there to the
  shoulders and continues below.
- `armup.png` — the cross is the **shoulder**. The arm runs right from it.
- `armfore.png` — the cross is the **elbow**. The forearm runs right from it.

**2. The magenta line is the bone.** It runs from the pivot to the far joint, at
the length that bone actually is. Draw the part so it covers that line — an
upper arm should reach the magenta circle, which is where the elbow will be.
Overshoot is fine and normal (a sleeve is wider than the bone inside it); what
matters is that the joint lands on the cross.

**3. Scale is fixed: 4 image pixels = 1 rig unit.** That is why the canvases are
the sizes they are. Draw at that size and everything fits together; the game
scales the whole character to the window afterwards.

## What stays procedural, and why

**The coat's flap is not in `coat.png`.** The coat's swinging hem is driven by
the cloth solver — it is the two chains in the `.odrig`, and it flaps because
the physics says so. A drawn coat is a rigid sheet, so `coat.png` covers the
part that does not move much and the hem keeps being drawn beneath it. If the
drawn coat looks wrong over the moving one, shorten `coat.png` rather than
lengthening it.

**Hands stay procedural for now.** A hand is not one drawing — it is four pose
classes (`relaxed`, `open`, `fist`, `point`) times a continuous facing from palm
to back of hand, which is why the engine builds them from geometry instead. If
you want to draw those too it is a bigger job and worth talking about first.

**The face stays procedural** — the eyes blink, track a gaze target, and the
mouth is rigged (`mouth` and `jaw` per pose). Draw `head.png` **without a face**
if you want to keep that, and the drawn head will have live eyes and a live
mouth on it. Draw one in if you would rather have a fixed expression, and it
will simply cover them.

## Testing it

1. Save your PNG in this folder under the name from the table.
2. Run the game, press **F10** at the main menu — that walks this character
   through all seven poses.
3. **F9** swaps to the bone skeleton if you need to see where a joint actually
   is; **F8** mirrors the body; right-drag moves it; the wheel scales it.

If a part comes out in the wrong place, it is almost always the pivot. Press F9
to see where the joint is, and compare it with where your cross was.

## If you want to change the numbers

The `part` lines are at the bottom of `../rig.odrig`:

```
part head    file art/head.png    pivot 0.500,0.812 turn 90 unit 4
```

`pivot` is fractions of the image, `turn` is how much to rotate your drawing to
lie along the bone (90 for upright, 0 for pointing right), and `unit` is pixels
per rig unit. Change `unit` if you would rather draw at a different resolution —
everything else scales from it.
