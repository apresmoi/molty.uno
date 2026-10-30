# shared by every helper: the config dir, its one-time migration, env defaults. Never prints the token.
#   ~/.config/molty/env    MOLTY_DEVICE=molty-… [MOLTY_RELAY=https://… only for a private relay]
#   ~/.config/molty/token  the agent token (0600)
# MOLTY_CONFIG_DIR overrides the dir; MOLTY_RELAY / MOLTY_DEVICE in the environment win over the file.
MOLTY_DEFAULT_RELAY=https://molty-relay.alicenet.workers.dev
C=${MOLTY_CONFIG_DIR:-$HOME/.config/molty}
OLD=$HOME/.config/molty-claude
if [ -z "${MOLTY_CONFIG_DIR:-}" ] && [ ! -e "$C/env" ] && [ ! -e "$C/token" ] && [ -d "$OLD" ]; then
  # moved from the session skill's old home: copy env + token once, keep the old dir untouched
  (umask 077; mkdir -p "$C") && for f in env token; do [ -e "$OLD/$f" ] && cp -p "$OLD/$f" "$C/$f"; done
  echo "molty: config copied from $OLD to $C (the old dir can go)" >&2
fi
_r=${MOLTY_RELAY:-}; _d=${MOLTY_DEVICE:-}
[ -f "$C/env" ] && . "$C/env"
R=${_r:-${MOLTY_RELAY:-$MOLTY_DEFAULT_RELAY}}; R=${R%/}; D=${_d:-${MOLTY_DEVICE:-}}
UA=molty-skill
