#!/system/bin/sh

[ "$API" -ge 29 ] || abort "SynClip requires Android 10 (API 29) or newer."

case "$ARCH" in
  arm64) synclip_abi=arm64-v8a ;;
  x64) synclip_abi=x86_64 ;;
  *) abort "SynClip requires a 64-bit ARM or x86 Android system." ;;
esac

synclip_payload="$MODPATH/payload/$synclip_abi"
[ -f "$synclip_payload/synclip-android-launcher" ] &&
  [ -f "$synclip_payload/libsynclip_android.so" ] ||
  abort "SynClip package does not contain $synclip_abi."
[ -s "$MODPATH/payload/synclip-pair.apk" ] && [ -s "$MODPATH/payload/synclip-pair.version" ] ||
  abort "SynClip package does not contain the pairing app."
synclip_pair_version=$(cat "$MODPATH/payload/synclip-pair.version") || abort "Cannot read the pairing app version."
case "$synclip_pair_version" in
  ''|0*|*[!0-9]*) abort "Invalid pairing app version." ;;
esac

mkdir -p "$MODPATH/bin" || abort "Cannot create the SynClip module directories."
mv "$synclip_payload/synclip-android-launcher" "$MODPATH/bin/" &&
  mv "$synclip_payload/libsynclip_android.so" "$MODPATH/bin/" &&
  mv "$MODPATH/payload/synclip.jar" "$MODPATH/bin/" &&
  mv "$MODPATH/payload/synclip-pair.apk" "$MODPATH/bin/" &&
  mv "$MODPATH/payload/synclip-pair.version" "$MODPATH/bin/" ||
  abort "Cannot install the SynClip payload."
set_perm "$MODPATH/bin/synclip-android-launcher" 0 0 0700
set_perm "$MODPATH/bin/synclip-pair.apk" 0 0 0600
set_perm "$MODPATH/bin/synclip-pair.version" 0 0 0600
set_perm "$MODPATH/action.sh" 0 0 0755

# The installer has already extracted the alternate ABI inside this module.
for synclip_arch_dir in "$MODPATH/payload/arm64-v8a" "$MODPATH/payload/x86_64"; do
  rm -f "$synclip_arch_dir/synclip-android-launcher" "$synclip_arch_dir/libsynclip_android.so"
  rmdir "$synclip_arch_dir" 2>/dev/null || true
done
rmdir "$MODPATH/payload" 2>/dev/null || true

ui_print "- Installed SynClip for $synclip_abi."
ui_print "- Reboot, then use SynClip's Action button to open the bundled QR scanner."
