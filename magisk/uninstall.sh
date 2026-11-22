#!/system/bin/sh

MODDIR=${0%/*}

if [ -r "$MODDIR/companion.sh" ]; then
  . "$MODDIR/companion.sh"
  synclip_pair_uninstall || /system/bin/log -t SynClip -p w 'Could not remove the module-owned pairing app.'
fi

if [ -d /data/local/synclip ] && [ ! -L /data/local/synclip ] && [ ! -L /data/local ]; then
  rm -f /data/local/synclip/synclip.jar /data/local/synclip/libsynclip_android.so /data/local/synclip/alert.mode
  rmdir /data/local/synclip 2>/dev/null || true
fi
if [ -d /data/adb/synclip ] && [ ! -L /data/adb/synclip ] &&
    [ "$(stat -c %u /data/adb/synclip)" = 0 ]; then
  rm -f /data/adb/synclip/config
  rmdir /data/adb/synclip 2>/dev/null || true
fi
