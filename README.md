# cpkg

One tool, for every* system.

Every Linux distro has its own package manager, and they all use different
words for the same things. `apt install` here, `dnf install` there, `pacman
-S` somewhere else. cpkg puts one small set of commands on top of whatever
you're running, so you don't have to remember which one you're on.

It also knows about flatpak, snap, and nix, and it can switch to the
"low-level" tool under your package manager when you need it (dpkg, rpm,
pkgtool, or the AUR through yay).

## The two commands

**cpkgsh** drops you into a small interactive shell:

```
$ cpkgsh
(APT/dpkg) /home/you$ install vim
(APT/dpkg) /home/you$ search htop
(APT/dpkg) /home/you$ exit
```

**cpkg** runs one command and gets out, no shell needed:

```
$ cpkg install vim
$ cpkg search htop
```

Both understand the same words:

```
install <pkg>    put a package on the system
remove <pkg>     take it off, keep its config
purge <pkg>      take it off, and its config too
update           refresh the package list
upgrade [pkg]    upgrade everything, or just one thing
search <term>    look for a package
show <pkg>       show details about a package
list             list what's installed
autoremove       clear out orphaned packages
clean            clear the local package cache
mode [name]      switch backend (see below)
config generate  save a list of installed packages
config load      install from a saved list
help             show all this again
```

## Switching backend

Type `mode` with nothing after it and it flips between your native
manager and its low-level counterpart:

- APT flips to dpkg
- DNF and Zypper flip to rpm
- Pacman flips to the AUR (through yay, if you have it)
- Slackpkg flips to pkgtool

You can also jump straight to flatpak, snap, or nix if any of those are
installed:

```
mode flatpak
mode nix
mode         # back to native
```

The AUR is community-submitted software, not reviewed by Arch. cpkg
prints a loud warning before switching you into it. Only install what
you'd trust from a stranger's PKGBUILD.

## Moving packages between machines

```
cpkg config generate my-packages.txt
```

writes out everything you have installed, tagged by where it came from
(your native manager, or flatpak/snap/nix). Take that file to another
machine and run:

```
cpkg config load my-packages.txt
```

It'll offer to set up flatpak, snap, or nix if the new machine doesn't
have them yet, then install what it can through the native manager. If
some packages aren't available that way, it asks whether to look for
them on flatpak, then nix, then snap, before giving up on them.

## Building it

You need a C compiler and readline's development headers.

```
sudo apt install libreadline-dev      # Debian, Ubuntu
sudo dnf install readline-devel       # Fedora
sudo pacman -S readline                # Arch
```

Then:

```
cc -O2 -Wall -o cpkgsh cpkg.c -lreadline
cc -O2 -Wall -DCPKG_BUILD -o cpkg cpkg.c -lreadline
```

Same source file, two binaries — a compile-time flag decides which one
you get.

## Or just install the .deb

```
./build-deb.sh
sudo apt install ./cpkg_1.0_amd64.deb
```

Run `build-deb.sh` on the machine you're installing to (or something
close to it) — it links against your system's own readline and glibc,
and a package built on one distro won't always run cleanly on another.

## A word of caution

Both binaries ask for your sudo password once and hang onto it for the
rest of the session, the same way `sudo -v` does. Nothing here fights
you for control of your terminal, and nothing runs commands you didn't
ask for — but as with any tool that calls `sudo` on your behalf, read
what it's about to do before you say yes.

## License

GPLv3. Full text in <a href="LICENSE">LICENSE</a>.
