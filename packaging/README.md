# Packages

arv installs with `make install` (README.md, "Install"); these build a package of it for a
distribution, so the package manager can remove it again and say what it installed. Both run
`make`, `make check` and `make install PREFIX=/usr`, and install:

| What | Where |
|---|---|
| `arv`, `arv-assist`, `arv-gui`, `udfwrite`, `bagit` | `/usr/bin` |
| arv's source, the tree every disc carries in `tools/` | `/usr/share/arv` |
| tab completion (it asks arv itself: `src/arv/completion/`) | bash: `/usr/share/bash-completion/completions/arv`; zsh: `site-functions/_arv` (Arch), `vendor-completions/_arv` (Debian) |

## Debian and Ubuntu

`debian/` builds a native package. It lives here, in `packaging/debian/`, so the top of the tree
stays tidy and a distribution's own `debian/` never collides with it; Debian's tools look for it at
the top, so it is copied there only while building:

```sh
sudo apt install build-essential debhelper git python3 7zip   # p7zip-full on older releases
just deb                                                      # or, by hand:
cp -r packaging/debian debian && dpkg-buildpackage -us -uc -b; rm -rf debian
sudo apt install ../arv_*.deb
```

`DEB_BUILD_OPTIONS=nocheck` skips `make check` (a few minutes).

## Arch Linux

`arch/PKGBUILD` is `arv-git`: the latest commit, as there are no releases yet.

```sh
cd packaging/arch && makepkg -si
```

## From a release tarball

`make install` works without git too: in a tree that `git archive` made (as GitHub's release
tarballs are), it installs that tree, less its build output, as `/usr/share/arv`, and takes the
commit from `COMMIT`, which `git archive` fills in (`export-subst`). A tarball made another way
records the commit as `unknown`.
