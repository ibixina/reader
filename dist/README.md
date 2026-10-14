# Ubuntu distribution

The `.deb` in this directory is built and tested on Ubuntu 24.04 for Intel/AMD
64-bit (`amd64`). Install it from the repository root:

```sh
sudo apt update
sudo apt install ./dist/paper-reader_*_ubuntu24.04_amd64.deb
paper-reader
```

Ubuntu installs the required Qt, WebEngine, Poppler, and SQLite runtime packages.
The installer includes the application, desktop menu entry, and icon.

Verify the download from this directory with:

```sh
sha256sum -c paper-reader_*_ubuntu24.04_amd64.deb.sha256
```

Rebuild on the target Ubuntu release and CPU architecture with
`./packaging/build-deb.sh` after installing the build dependencies listed in the
repository README.
