#!/system/bin/sh

SYNCLIP_PAIR_PACKAGE=synclip.pair
SYNCLIP_PAIR_STATE=/data/adb/synclip
SYNCLIP_PAIR_RECEIPT=$SYNCLIP_PAIR_STATE/pair-app.receipt
SYNCLIP_PAIR_APK=$MODDIR/bin/synclip-pair.apk
SYNCLIP_PAIR_VERSION=$MODDIR/bin/synclip-pair.version

synclip_pair_info() (
  listed=$(/system/bin/pm list packages -f --show-versioncode --user 0 "$SYNCLIP_PAIR_PACKAGE") || exit 1
  printf '%s\n' "$listed" | sed -n 's/^package:\(.*\)=synclip\.pair versionCode:\([0-9][0-9]*\)$/\2 \1/p'
)

synclip_pair_private_state() {
  [ -d "$SYNCLIP_PAIR_STATE" ] && [ ! -L "$SYNCLIP_PAIR_STATE" ] &&
    [ "$(stat -c '%u:%a' "$SYNCLIP_PAIR_STATE")" = '0:700' ]
}

synclip_pair_receipt() (
  if synclip_pair_private_state && [ -f "$SYNCLIP_PAIR_RECEIPT" ] &&
      [ ! -L "$SYNCLIP_PAIR_RECEIPT" ]; then
    cat "$SYNCLIP_PAIR_RECEIPT"
  fi
)

synclip_pair_install() (
  if [ ! -s "$SYNCLIP_PAIR_APK" ] || ! expected=$(cat "$SYNCLIP_PAIR_VERSION"); then
    printf '%s\n' 'The module is missing its pairing app payload.' >&2
    exit 1
  fi
  case "$expected" in
    ''|*[!0-9]*) printf '%s\n' 'Invalid pairing app version.' >&2; exit 1 ;;
  esac

  if ! before=$(synclip_pair_info); then
    printf '%s\n' 'Android Package Manager is unavailable.' >&2
    exit 1
  fi
  if [ -n "$before" ] && [ "${before%% *}" -ge "$expected" ]; then
    exit 0
  fi

  replace=-r
  keep_receipt=false
  if [ -z "$before" ]; then
    # Do not claim an app installed concurrently by the user or another module.
    replace=-R
    keep_receipt=true
  elif [ "$before" = "$(synclip_pair_receipt)" ]; then
    keep_receipt=true
  fi

  if ! size=$(stat -c %s "$SYNCLIP_PAIR_APK"); then
    exit 1
  fi
  if result=$(cat "$SYNCLIP_PAIR_APK" | /system/bin/pm install "$replace" --user 0 \
      --pkg "$SYNCLIP_PAIR_PACKAGE" -S "$size" - 2>&1); then
    if ! after=$(synclip_pair_info) || [ -z "$after" ] || [ "${after%% *}" -lt "$expected" ]; then
      printf '%s\n' 'Android did not report the expected installed pairing app.' >&2
      exit 1
    fi
  else
    # An overlapping Action or boot hook may have completed the same install.
    if after=$(synclip_pair_info) && [ -n "$after" ] && [ "${after%% *}" -ge "$expected" ]; then
      exit 0
    fi
    printf '%s\n' "$result" >&2
    exit 1
  fi

  if "$keep_receipt" && [ "${after%% *}" = "$expected" ]; then
    if ! (umask 077; mkdir -p "$SYNCLIP_PAIR_STATE") || ! synclip_pair_private_state ||
        ! temp=$(mktemp "$SYNCLIP_PAIR_STATE/.pair-app.XXXXXX"); then
      printf '%s\n' 'The pairing app installed, but the module could not record ownership.' >&2
      exit 0
    fi
    if ! printf '%s\n' "$after" > "$temp" || ! mv -f "$temp" "$SYNCLIP_PAIR_RECEIPT"; then
      rm -f "$temp"
      printf '%s\n' 'The pairing app installed, but the module could not record ownership.' >&2
    fi
  fi
)

synclip_pair_uninstall() (
  saved=$(synclip_pair_receipt)
  [ -n "$saved" ] || exit 0
  current=$(synclip_pair_info) || exit 1
  if [ "$current" = "$saved" ]; then
    /system/bin/pm uninstall --user 0 --versionCode "${saved%% *}" "$SYNCLIP_PAIR_PACKAGE" || exit 1
  fi
  rm -f "$SYNCLIP_PAIR_RECEIPT"
)
