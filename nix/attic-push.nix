{
  pkgs,
  attrs,
  flakeRef,
  tokenFile ? null,
  endpoint ? "https://cache.modulixos.org:8443",
  cache ? "modulix",
  systems ? [ "x86_64-linux" "aarch64-linux" ],
}:

let
  lib = pkgs.lib;
  server = "modulix";
  defaultTokenFile = if tokenFile == null then "" else "${tokenFile}";
in
pkgs.writeShellApplication {
  name = "mx-push-cache";

  runtimeInputs = [
    pkgs.attic-client
    pkgs.rage
    pkgs.nix
    pkgs.coreutils
  ];

  text = ''
    endpoint=''${ATTIC_ENDPOINT:-${endpoint}}
    cache=''${ATTIC_CACHE:-${cache}}
    identity=''${ATTIC_IDENTITY:-$HOME/.ssh/id_ed25519}
    token_file=''${ATTIC_TOKEN_FILE:-${defaultTokenFile}}
    public_key=''${ATTIC_PUBLIC_KEY:-}
    flake=''${MX_PUSH_FLAKE:-${flakeRef}}
    dry_run=''${MX_PUSH_DRY_RUN:-0}

    read -r -a push_systems <<<"''${MX_PUSH_SYSTEMS:-${toString systems}}"
    push_attrs=( ${lib.escapeShellArgs attrs} )

    if [ -z "$token_file" ]; then
      echo "mx-push-cache: no deploy token in this flake." >&2
      echo "  create it with 'agenix -e secrets/attic-token.age', then 'git add secrets/attic-token.age'," >&2
      echo "  or point ATTIC_TOKEN_FILE at an age-encrypted token." >&2
      exit 1
    fi

    if [ ! -r "$token_file" ]; then
      echo "mx-push-cache: token file is not readable: $token_file" >&2
      exit 1
    fi

    if [ ! -r "$identity" ]; then
      echo "mx-push-cache: age identity is not readable: $identity" >&2
      echo "  override it with ATTIC_IDENTITY." >&2
      exit 1
    fi

    umask 077
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT

    XDG_CONFIG_HOME="$tmp/config"
    export XDG_CONFIG_HOME
    mkdir -p "$XDG_CONFIG_HOME/attic"

    token=$(rage --decrypt --identity "$identity" "$token_file" | tr -d '\r\n')
    if [ -z "$token" ]; then
      echo "mx-push-cache: decrypted token is empty: $token_file" >&2
      exit 1
    fi

    {
      printf 'default-server = "%s"\n\n' "${server}"
      printf '[servers.%s]\n' "${server}"
      printf 'endpoint = "%s"\n' "$endpoint"
      printf 'token = "%s"\n' "$token"
    } >"$XDG_CONFIG_HOME/attic/config.toml"
    unset token

    build_args=( -L --no-link --print-out-paths )
    if [ -n "$public_key" ]; then
      build_args+=(
        --extra-substituters "$endpoint/$cache"
        --extra-trusted-public-keys "$public_key"
      )
    fi

    outs="$tmp/outs"
    : >"$outs"

    for system in "''${push_systems[@]}"; do
      echo "mx-push-cache: building ${toString attrs} for $system" >&2
      targets=()
      for attr in "''${push_attrs[@]}"; do
        targets+=( "$flake#packages.$system.$attr" )
      done
      nix build "''${build_args[@]}" "''${targets[@]}" >>"$outs"
    done

    if [ ! -s "$outs" ]; then
      echo "mx-push-cache: no store path was produced" >&2
      exit 1
    fi

    count=$(wc -l <"$outs")

    if [ "$dry_run" != "0" ]; then
      echo "mx-push-cache: MX_PUSH_DRY_RUN set, not pushing these $count paths:" >&2
      cat "$outs" >&2
      exit 0
    fi

    echo "mx-push-cache: pushing $count paths to ${server}:$cache" >&2
    attic push --stdin "${server}:$cache" <"$outs"
  '';

  meta = {
    description = "Build ${toString attrs} for ${toString systems} and push them to the Modulix Attic cache";
    platforms = pkgs.lib.platforms.linux;
  };
}
