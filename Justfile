# Justfile — gnome-software-plugin-modulix. Run inside `nix develop`.

build_dir := "build"
prefix    := env_var_or_default("HOME", "/tmp") + "/.local"

# Configure + build (meson drives cargo + cbindgen for the Rust backend).
build:
    #!/usr/bin/env bash
    set -euo pipefail
    sub=$(basename "$(pkg-config --variable=plugindir gnome-software)")
    meson setup {{build_dir}} \
        --prefix={{prefix}} \
        --buildtype=debugoptimized \
        -Dplugindir={{prefix}}/lib/gnome-software/$sub \
        --reconfigure
    ninja -C {{build_dir}}

# Install the plugin under ~/.local for local testing.
install-dev: build
    ninja -C {{build_dir}} install
    @echo "✓ installed under {{prefix}}/lib/gnome-software/"

uninstall-dev:
    ninja -C {{build_dir}} uninstall

# Launch GNOME Software (bubblewrap) with the locally-built plugin.
run-gs: install-dev
    G_MESSAGES_DEBUG=GsPluginModulix gnome-software-dev --verbose

# Same, but pipe everything and keep only relevant lines.
run-gs-log: install-dev
    G_MESSAGES_DEBUG=all gnome-software-dev --verbose 2>&1 \
        | grep -iE "modulix|GsPlugin|error|warning|crash|segfault"

# Backend crate: test / lint / format.
test:
    cargo test --manifest-path backend/Cargo.toml

lint:
    cargo clippy --manifest-path backend/Cargo.toml -- -D warnings

fmt:
    cargo fmt --manifest-path backend/Cargo.toml

# Build the combined gnome-software + plugin package via the flake.
build-package:
    nix build .#default

# Confirm the entry-point symbol is exported.
check-plugin: install-dev
    #!/usr/bin/env bash
    set -euo pipefail
    so=$(echo {{prefix}}/lib/gnome-software/plugins-*/libgs_plugin_modulix.so)
    echo "plugin: $so"
    # grep without -q: -q exits on first match and SIGPIPEs nm, which under
    # `set -o pipefail` falsely fails the check on a large symbol table.
    nm -D "$so" | grep gs_plugin_query_type >/dev/null \
        && echo "✓ gs_plugin_query_type present" \
        || echo "✗ gs_plugin_query_type MISSING"

clean:
    rm -rf {{build_dir}}
    cargo clean --manifest-path backend/Cargo.toml
