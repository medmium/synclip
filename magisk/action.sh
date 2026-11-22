#!/system/bin/sh

MODDIR=${0%/*}

if [ -f "$MODDIR/disable" ] || [ -f "$MODDIR/remove" ]; then
  printf '%s\n' 'Enable the SynClip module before opening pairing.' >&2
  exit 1
fi

. "$MODDIR/companion.sh" || exit 1
synclip_pair_install || exit 1
exec /system/bin/am start --user 0 -n "$SYNCLIP_PAIR_PACKAGE/.PairActivity"
