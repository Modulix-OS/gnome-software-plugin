{
  description = "GNOME Software plugin for Modulix-OS (nix packages, modules, plugins)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    modulix-store-client = {
      url = "git+file:///home/quentin/Programmes/Modulix-OS/modulix-store-client";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, rust-overlay, modulix-store-client }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs { inherit system overlays; };

        rustToolchain = pkgs.rust-bin.stable.latest.default.override {
          extensions = [ "rust-src" "rust-analyzer" "clippy" "rustfmt" ];
        };

        # gnome-software patched with an explicit "GnomeSoftware::SortKey" metadata
        # sort key, read *before* the packaging-format-preference GSetting in the
        # details-page version-selector sort. Lets the Modulix plugin impose
        # module > Flatpak > nix-variant ordering regardless of user settings.
        gnome-software-patched = pkgs.gnome-software.overrideAttrs (old: {
          patches = (old.patches or [ ]) ++ [ ./nix/gs-details-page-sortkey.patch ];
        });

        # ─── The plugin (.so) ───────────────────────────────────────────────
        plugin = pkgs.stdenv.mkDerivation {
          pname = "gnome-software-plugin-modulix";
          version = "0.1.0";
          src = ./.;

          cargoDeps = pkgs.rustPlatform.importCargoLock {
            lockFile = "${modulix-store-client}/Cargo.lock";
          };
          cargoRoot = "../modulix-store-client";

          nativeBuildInputs = [
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            pkgs.cargo
            pkgs.rustc
            pkgs.rustPlatform.cargoSetupHook
            pkgs.rust-cbindgen
            pkgs.gettext # msgfmt: compile po/*.po → *.mo
            pkgs.glib # glib-compile-resources: embed the badge icon
          ];

          buildInputs = [
            pkgs.gnome-software
            pkgs.glib
            pkgs.gtk4
            pkgs.libadwaita
            pkgs.appstream
            pkgs.json-glib
            pkgs.libsoup_3
            pkgs.libxmlb
          ];

          postUnpack = ''
            cp -r --no-preserve=mode,ownership \
              ${modulix-store-client} "$NIX_BUILD_TOP/modulix-store-client"
          '';

          env.CARGO_NET_OFFLINE = "true";

          meta = {
            description = "GNOME Software plugin to browse/install Modulix nix packages and modules";
            platforms = pkgs.lib.platforms.linux;
          };
        };

        # ─── gnome-software bundled with the plugin (single package) ─────────
        # The plugin loader uses a compiled-in plugin dir; there is no env var to
        # add one, so the plugin .so is copied into gnome-software's own
        # plugins-<api> directory. ABI matches: both derive from the same
        # pkgs.gnome-software.
        gnome-software-modulix = gnome-software-patched.overrideAttrs (old: {
          postInstall = (old.postInstall or "") + ''
            for d in ${plugin}/lib/gnome-software/plugins-*; do
              install -Dm755 "$d"/*.so -t "$out/lib/gnome-software/$(basename "$d")/"
            done
            # Ship the plugin's translations alongside gnome-software's own data
            # (the badge icon is embedded in the .so as a GResource).
            if [ -d "${plugin}/share/locale" ]; then
              mkdir -p "$out/share/locale"
              cp -r "${plugin}/share/locale/." "$out/share/locale/"
            fi
          '';
        });

        # ─── Dev runner: gnome-software with the locally-built plugin ────────
        # bubblewrap overlays a tmp plugin dir (store plugins + our local .so)
        # over the read-only store plugin dir, so no rebuild/root is needed.
        gnome-software-dev = pkgs.writeShellScriptBin "gnome-software-dev" ''
          set -e
          gs_plugindir=$(echo ${gnome-software-patched}/lib/gnome-software/plugins-*)
          local_so=$(echo "''${HOME}"/.local/lib/gnome-software/plugins-*/libgs_plugin_modulix.so)

          if [ ! -f "''${local_so}" ]; then
            echo "[dev] plugin not found under ~/.local — run 'just install-dev' first." >&2
            exit 1
          fi

          tmp=$(mktemp -d)
          cp "''${gs_plugindir}"/*.so "''${tmp}"/
          cp "''${local_so}" "''${tmp}"/
          echo "[dev] overlaying $(ls "''${tmp}" | wc -l) plugins onto ''${gs_plugindir}"

          exec ${pkgs.bubblewrap}/bin/bwrap \
            --ro-bind / / \
            --dev /dev --proc /proc --tmpfs /tmp \
            --bind "''${HOME}" "''${HOME}" \
            --bind /run/user /run/user \
            --ro-bind-try /var/lib/flatpak /var/lib/flatpak \
            --bind "''${tmp}" "''${gs_plugindir}" \
            ${gnome-software-patched}/bin/gnome-software "$@"
        '';

      in
      {
        packages = {
          inherit plugin;
          default = gnome-software-modulix;
          gnome-software-modulix = gnome-software-modulix;
        };

        devShells.default = pkgs.mkShell {
          name = "gnome-software-plugin-modulix";
          nativeBuildInputs = [
            pkgs.meson pkgs.ninja pkgs.pkg-config pkgs.cmake pkgs.gettext
          ];
          buildInputs = [
            pkgs.gnome-software
            pkgs.glib pkgs.glib.dev
            pkgs.gtk4 pkgs.gtk4.dev
            pkgs.libadwaita pkgs.libadwaita.dev
            pkgs.appstream pkgs.appstream.dev
            pkgs.json-glib pkgs.json-glib.dev
            pkgs.libsoup_3 pkgs.libsoup_3.dev
            pkgs.libxmlb pkgs.libxmlb.dev
            pkgs.openssl pkgs.openssl.dev pkgs.zlib

            rustToolchain
            pkgs.rust-cbindgen
            pkgs.just pkgs.jq pkgs.git
            pkgs.d-spy pkgs.bustle pkgs.gdb
            gnome-software-dev
            pkgs.bubblewrap
          ];

          shellHook = ''
            export PKG_CONFIG_PATH="${gnome-software-patched}/lib/pkgconfig:$PKG_CONFIG_PATH"
            export GSETTINGS_SCHEMA_DIR="${gnome-software-patched}/share/gsettings-schemas/${gnome-software-patched.name}:$GSETTINGS_SCHEMA_DIR"
            export G_MESSAGES_DEBUG="GsPluginModulix"
            echo "gnome-software-plugin-modulix dev shell — gnome-software $(pkg-config --modversion gnome-software 2>/dev/null)"
            echo "  just build / just install-dev / just run-gs"
          '';
        };
      });
}
