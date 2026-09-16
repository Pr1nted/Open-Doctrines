# Google Play: what stands between the APK and a listing

**Why this is worth doing at all:** 239 Android downloads in the 30 days to 15
September 2026, second only to Windows (276), every one of them a sideload from
an itch.io APK. People are getting past an unknown-sources warning to play
this. There is no Play listing and no F-Droid entry.

**Status: nothing here is done.** Two of the blockers are real engineering, and
one is a decision only you can make.

---

## The blockers, in the order they bite

### 1. The signing key is a throwaway — **your decision, and it is permanent**

`tools/package_android.sh` generates a debug keystore on the fly if one is
missing, with `storepass android`, and signs with that:

```bash
keytool -genkeypair -keystore "$KS" -alias od -storepass android -keypass android ...
```

That is correct for itch, where nothing verifies the publisher. Play is
different: the upload key identifies you forever, and **an app signed with a
lost key can never be updated again** — you would have to publish a new
listing and lose every install and review.

So this one is yours: generate a real upload keystore, put it somewhere it
cannot be lost, and give CI its password as a secret. Do not paste the
password anywhere it could be logged, and do not commit the keystore.
Play App Signing (where Google holds the release key and you keep only an
upload key) is the safer arrangement and is the default for new apps — take it.

### 2. Play needs an **AAB**, and this project has no Gradle — **real work**

New apps have had to publish an Android App Bundle rather than an APK since
2021. `package_android.sh` deliberately has no Gradle (`android:hasCode="false"`,
no Java at all), which is a good decision that happens to make the usual AAB
path unavailable.

The non-Gradle route exists and is roughly:

1. `aapt2 link --proto-format` instead of the current binary-format link, to
   produce protobuf resources.
2. Repack into the bundle layout — `base/manifest/AndroidManifest.xml`,
   `base/res/`, `base/lib/`, `base/assets/`, `base/resources.pb`.
3. `bundletool build-bundle --modules=base.zip --output=app.aab`.
4. `bundletool build-apks --connected-device` to prove the bundle installs.

**This has not been written, and deliberately so:** `bundletool` is not
installed on this machine, so a script for it could not be run even once, and
untested build tooling that looks right is worse than none. Install
`bundletool`, then it is an afternoon.

Worth knowing before starting: an AAB also lets Play split by ABI, so adding
`armeabi-v7a` later costs users nothing in download size.

### 3. `targetSdkVersion` will need raising — **small, but check the date**

`android/AndroidManifest.xml` targets SDK 34. Play requires new apps to target
an API level within about a year of the current Android release, and that
threshold moves every August. **Check the current requirement in the Play
Console before building** — it is a one-line manifest change plus a real
device test, not a rewrite, but it is a hard gate.

### 4. The testing requirement — **check whether it applies to you**

Personal (non-organisation) developer accounts created in recent years have had
to run a closed test with a minimum number of testers for a continuous period
before they may launch publicly. The exact numbers have changed more than once,
so read the current rule in the Play Console rather than trusting any figure
here.

If it does apply: **the Discord server is the tester pool.** Fifteen members is
about the order of magnitude such rules ask for, which makes move 3 of
[README.md](README.md) a prerequisite for this rather than a separate project.

### 5. The $25 one-time registration fee — yours to pay

---

## What the listing needs, and what already exists

| Asset | Requirement | Where it is |
|---|---|---|
| Privacy policy URL | required | `https://opendoctrines.pages.dev/privacy` ✅ |
| App icon | 512×512 PNG | `android/res/` has the launcher icon; needs a 512 export |
| Feature graphic | 1024×500 PNG | **not made** |
| Phone screenshots | 2–8, 16:9 landscape | press kit has stills; check they are from the Android build |
| Short description | ≤80 characters | in [store-copy.md](store-copy.md) |
| Full description | ≤4,000 characters | in [store-copy.md](store-copy.md) |
| Content rating | questionnaire | war/strategy themes, no gore, no chat with strangers in the base game — **note the multiplayer lobby chat when answering** |
| Data safety form | declaration | the game collects nothing without consent; multiplayer sends an account ID and an invite code. Answer from `net/PRIVACY.md`, which already says this precisely |
| Ads declaration | none | there are no ads and there is nothing to buy |

## The order to do it in

1. Decide on Play App Signing and make the upload key. *(You.)*
2. Check the current `targetSdk` and testing requirements in the Console.
3. Install `bundletool`, write and **run** the AAB path, prove it installs on a
   real device with `bundletool build-apks --connected-device`.
4. Make the feature graphic and the 512 icon.
5. Closed test, with the Discord as the pool.
6. Launch, and add the Play badge to the itch page and the site.

F-Droid is a separate, cheaper path worth doing in parallel: it wants a
reproducible build from source and a metadata file in their repository, takes
no fee, and reaches exactly the audience that sideloads APKs — which is
demonstrably 239 people a month here.
