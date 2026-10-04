# The Ko-fi page

`ko-fi.com/pr1nted`. Linked from `.github/FUNDING.yml`, the README and every
site footer.

`cover.png` is the page's cover image: 1500x500, which is the 3:1 Ko-fi asks
for, cropped from `docs/steam/banner-steam-hero.png`. Regenerate it with the
snippet at the bottom if the hero changes.

**Uploading it is a manual step.** Ko-fi's cover uploader is a Dropzone whose
hidden `<input type=file>` is appended to `document.body` and never reaches the
accessibility tree, so the only way in is the native file picker — which
browser automation cannot drive. Cover → "Click or drop files here" → pick
this file.

Still to do on the page, in the order that matters:

1. **Connect payments** (Stripe or PayPal). Until this is done the page carries
   an "Action required" banner and cannot accept anything. Bank details, so it
   is a person's job and nobody else's.
2. The cover above.
3. A pinned post introducing the page.
4. A goal, if you want one — it is a public number, so it is a choice rather
   than a gap.

```python
from PIL import Image
src = Image.open('docs/steam/banner-steam-hero.png').convert('RGB')
W, H = 1500, 500
s = src.resize((W, round(src.height * W / src.width)), Image.LANCZOS)
top = (s.height - H) // 2
s.crop((0, top, W, top + H)).save('docs/kofi/cover.png')
```
