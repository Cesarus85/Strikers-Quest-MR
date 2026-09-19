# APK

`strikers-android-1.2.0-arm64.apk` — arm64-v8a, Android 9 (API 28) or later, Vulkan required.

**No game data is included.** The app will not start the game until you supply your own copy of the
Super Mario Strikers disc: a `.iso`, `.gcm`, `.ciso` or `.gcz` image, or the extracted disc's
`files` folder. The launcher shows the exact folder to put it in, and offers to copy that path.

Verify what you downloaded before installing it:

```sh
sha256sum -c strikers-android-1.2.0-arm64.apk.sha256
```

## About the signature

This APK is signed with Android's **debug key** — the one every SDK install shares. That is enough
for the system to install it, and it is not enough to prove who built it: anyone can sign anything
with the same key. Treat the checksum above, and the fact that you can rebuild this file from the
source in this repository, as the things that say where it came from.

Because the key is shared, a later APK signed with a *different* key will refuse to install over
this one. Uninstall first if that happens; the game data lives in `/Strikers` on shared storage and
is not touched by an uninstall.

## Building it yourself

See [`../smstrikers-port/README-android.md`](../smstrikers-port/README-android.md). Two commands:
`tools/build-android.sh`, then `cd android && ./gradlew assembleRelease`.
