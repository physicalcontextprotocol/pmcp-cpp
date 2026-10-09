# vcpkg port for `pmcp` (draft)

These files are the proposed [vcpkg](https://vcpkg.io) port for `pmcp-cpp`.
They live here for review; to actually make `vcpkg install pmcp` work for
everyone, they must be merged into [microsoft/vcpkg](https://github.com/microsoft/vcpkg)
as a PR — a port is only "published" once it is in that registry.

## Files

| File | Purpose |
|---|---|
| `vcpkg.json` | Port manifest: name, version, license, host deps. |
| `portfile.cmake` | Fetches the release tag, configures/installs, fixes up the CMake config. |
| `usage` | The "how do I link this" note vcpkg prints after install. |

`portfile.cmake` pins `REF v1.0.1` and matches it with the `SHA512` of the
GitHub source archive for that tag. **When you cut a new release, regenerate
both** — see below.

## Submitting (one-time)

1. Fork `microsoft/vcpkg`.
2. Copy these three files into `ports/pmcp/` in your fork.
3. From the vcpkg checkout:
   ```sh
   ./bootstrap-vcpkg.sh          # or .\bootstrap-vcpkg.bat on Windows
   vcpkg x-add-version pmcp --overwrite-version
   ```
   This fills in `versions/p-/pmcp.json` and the `versions/baseline.json` entry.
4. Commit, push, open a PR titled `[pmcp] Add new port (1.0.1)`.
5. CI builds the port on all three platforms. Address any failures, then it
   gets reviewed and merged.

## Keeping it current

On each new release, update `vcpkg.json`'s `version` and `portfile.cmake`'s
`REF`, then recompute the hash:

```sh
curl -sL https://github.com/physicalcontextprotocol/pmcp-cpp/archive/refs/tags/vX.Y.Z.tar.gz \
  | sha512sum        # or: shasum -a 512
```

Paste the digest into `SHA512`, re-run `vcpkg x-add-version pmcp
--overwrite-version`, and open an update PR.

## Testing the port locally before the PR

```sh
vcpkg install pmcp --overlay-ports=/path/to/pmcp-cpp/packaging/vcpkg
```

## Alternatives (no PR required)

- **`FetchContent` / `add_subdirectory`** — vendor the source directly; works today.
- **Conan** — a `conan-center-index` recipe is a similar reviewed-PR process;
  `conan create` works locally first.
- **CPack** — self-hosted `.tar.gz`/`.deb`/`.rpm` from the CMake project, no registry.
