#!/system/bin/sh

MODDIR=${0%/*}
RUNTIME_DIR=/data/local/synclip
CONFIG=/data/adb/synclip/config

log_status() {
  /system/bin/log -t SynClip -p "$1" "$2"
}

module_enabled() {
  [ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ]
}

while [ "$(/system/bin/getprop sys.boot_completed)" != 1 ]; do
  module_enabled || exit 0
  sleep 2
done

module_enabled || exit 0
if [ -r "$MODDIR/companion.sh" ]; then
  . "$MODDIR/companion.sh"
  if ! pairing_result=$(synclip_pair_install 2>&1); then
    log_status w "Pairing app installation failed. Open the module's Action to retry. $pairing_result"
  elif [ -n "$pairing_result" ]; then
    log_status w "$pairing_result"
  fi
else
  log_status w 'The module is missing its pairing app installer.'
fi

if [ ! -s "$CONFIG" ]; then
  log_status i "Waiting for $CONFIG"
fi
while [ ! -s "$CONFIG" ]; do
  module_enabled || exit 0
  sleep 5
done

# Shell UID can read the DEX and native library after the launcher drops root.
# The stock /data/local parent and this directory are writable only by root.
if [ -L /data/local ] || [ "$(stat -c %u /data/local)" != 0 ]; then
  log_status e "Unexpected ownership of /data/local"
  exit 1
fi
case "$(stat -c %a /data/local)" in
  ?[0145][0145]) ;;
  *) log_status e "Unsafe permissions on /data/local"; exit 1 ;;
esac

if [ -L "$RUNTIME_DIR" ] || { [ -e "$RUNTIME_DIR" ] && [ ! -d "$RUNTIME_DIR" ]; }; then
  log_status e "Unexpected type for $RUNTIME_DIR"
  exit 1
fi
if ! (umask 077; mkdir -p "$RUNTIME_DIR") ||
    [ "$(stat -c %u "$RUNTIME_DIR")" != 0 ] ||
    ! chmod 0700 "$RUNTIME_DIR" ||
    ! chown 0:2000 "$RUNTIME_DIR" || ! chmod 0750 "$RUNTIME_DIR"; then
  log_status e "Cannot prepare $RUNTIME_DIR"
  exit 1
fi

for asset in synclip.jar libsynclip_android.so; do
  temp=$(mktemp "$RUNTIME_DIR/.$asset.XXXXXX") || exit 1
  if ! cp "$MODDIR/bin/$asset" "$temp" || ! chown 0:2000 "$temp" ||
      ! chmod 0440 "$temp" || ! rm -f "$RUNTIME_DIR/$asset" ||
      ! mv "$temp" "$RUNTIME_DIR/$asset"; then
    rm -f "$temp"
    log_status e "Cannot stage $asset"
    exit 1
  fi
done

# A private pipe keeps diagnostics in logcat while the supervisor tracks the
# actual launcher PID across its exec into Android's runtime.
supervisor=$(mktemp -d "$RUNTIME_DIR/.supervisor.XXXXXX") || exit 1
diagnostics="$supervisor/diagnostics"
if ! mkfifo "$diagnostics"; then
  rmdir "$supervisor"
  exit 1
fi
runner=
reader=

stop_runner() {
  if [ -n "$runner" ]; then
    if kill -0 "$runner" 2>/dev/null; then
      kill "$runner" 2>/dev/null || true
      sleep 1
      kill -0 "$runner" 2>/dev/null && kill -KILL "$runner" 2>/dev/null
    fi
    wait "$runner" 2>/dev/null
    runner=
  fi
  if [ -n "$reader" ]; then
    kill "$reader" 2>/dev/null || true
    wait "$reader" 2>/dev/null
    reader=
  fi
}

cleanup() {
  stop_runner
  rm -f "$diagnostics"
  rmdir "$supervisor" 2>/dev/null
}
trap cleanup 0
trap 'exit 0' HUP INT TERM

config_stamp() {
  stat -c '%d:%i:%s:%Y' "$CONFIG" 2>/dev/null
}

while module_enabled; do
  if [ -s "$CONFIG" ]; then
    stamp=$(config_stamp)
    (while IFS= read -r line; do log_status i "$line"; done) < "$diagnostics" &
    reader=$!
    (exec "$MODDIR/bin/synclip-android-launcher" "$RUNTIME_DIR/synclip.jar" \
      "$RUNTIME_DIR/libsynclip_android.so" "$CONFIG") > "$diagnostics" 2>&1 &
    runner=$!
    while module_enabled && kill -0 "$runner" 2>/dev/null && [ "$(config_stamp)" = "$stamp" ]; do
      sleep 2
    done
    stop_runner
    module_enabled || break
    if [ "$(config_stamp)" != "$stamp" ]; then
      log_status i "Pairing configuration changed. Reloading."
      continue
    fi
    log_status w "Clipboard service exited. Retrying in five seconds."
  fi
  sleep 5
done
