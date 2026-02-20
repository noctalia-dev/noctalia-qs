# noctalia-qs

**noctalia-qs** is a custom fork of [Quickshell](https://quickshell.org) — a flexible QtQuick-based desktop shell toolkit for Wayland. It serves as the shell framework powering [Noctalia Shell](https://github.com/noctalia-dev/noctalia-shell).

## What is noctalia-qs?

noctalia-qs extends Quickshell with features and patches specific to the Noctalia shell ecosystem, including:

- `ext-background-effect-v1` Wayland protocol support
- Noctalia-specific build defaults and configuration

The binary is named `noctalia-qs` and is a drop-in replacement for `quickshell` when using Noctalia Shell.

## Credits

noctalia-qs is built on top of **Quickshell**, developed by [outfoxxed](https://git.outfoxxed.me/outfoxxed) and contributors.

- Quickshell website: https://quickshell.org
- Quickshell source: https://git.outfoxxed.me/quickshell/quickshell
- Quickshell mirror: https://github.com/quickshell-mirror/quickshell

All credit for the core framework goes to the Quickshell project and its contributors.

## Building

See [BUILD.md](BUILD.md) for build instructions.

```bash
cmake -GNinja -B build-release -DCMAKE_BUILD_TYPE=Release noctalia-qs
cmake --build build-release
# binary: build-release/src/noctalia-qs
```

## License

Licensed under the GNU LGPL 3, same as the upstream Quickshell project.

Unless you explicitly state otherwise, any contribution submitted for inclusion shall be licensed as above, without any additional terms or conditions.
