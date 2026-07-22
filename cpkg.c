/*
 * cpkgsh / cpkg -- a unified shell (and one-shot runner) for whatever
 * native package manager is on the system.
 *
 * This single source builds two binaries (see the CPKG_BUILD macro
 * below): cpkgsh, an interactive REPL that maps a small set of verbs
 * (install/remove/purge/update/upgrade/search/show/list/autoremove/clean)
 * onto whichever backend is active (native manager, its low-level
 * counterpart, or flatpak/snap/nix), with `mode` switching and
 * `config generate/load` for cross-system package lists; and cpkg, a
 * one-shot runner for the same verbs (`cpkg install vim` and so on).
 *
 * Copyright (C) 2026 Damian Daniel <damian@danielovci.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Build cpkgsh: cc -O2 -Wall -o cpkgsh cpkg.c -lreadline
 * Build cpkg:   cc -O2 -Wall -DCPKG_BUILD -o cpkg cpkg.c -lreadline
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAXARGS 64
#define MAXLINE 4096

/* This single source builds two binaries. By default (no CPKG_BUILD
 * defined) it builds "cpkgsh", the interactive shell. Compiled again with
 * -DCPKG_BUILD it builds "cpkg", the one-shot runner (`cpkg install vim`
 * and so on). PROG_NAME is a plain string literal so it can be
 * concatenated into other literals at compile time, e.g.
 * PROG_NAME ": some message\n". */
#ifdef CPKG_BUILD
#define PROG_NAME "cpkg"
#else
#define PROG_NAME "cpkgsh"
#endif

static char PACKAGE_MANAGER[16];  /* "apt","dnf","zypper","pacman","slackpkg" */
static char PKG_MODE[16];         /* "native","dpkg","rpm","aur","pkgtool","flatpak","snap","nix" */
static pid_t MAIN_PID;
#ifndef CPKG_BUILD
static pid_t KEEPALIVE_PID = -1;
#endif

/* ---------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */

static int have_cmd(const char *cmd) {
    char buf[256];
    snprintf(buf, sizeof buf, "command -v %s >/dev/null 2>&1", cmd);
    int rc = system(buf);
    return rc == 0;
}

static int run_argv(char *argv[]) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, PROG_NAME ": failed to run '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Run `prefix... + args[0..nargs)` via execvp (no shell involved, so no
 * quoting/injection concerns from user-typed package names). */
static int run_with_args(char **prefix, int nprefix, char **args, int nargs) {
    char *argv[MAXARGS];
    int i = 0, n;
    for (n = 0; n < nprefix && i < MAXARGS - 1; n++) argv[i++] = prefix[n];
    for (n = 0; n < nargs && i < MAXARGS - 1; n++) argv[i++] = args[n];
    argv[i] = NULL;
    return run_argv(argv);
}

#define RUN_PREFIXED(args, nargs, ...) ({ \
    char *_pre[] = { __VA_ARGS__ }; \
    run_with_args(_pre, sizeof(_pre)/sizeof(_pre[0]), (args), (nargs)); \
})

/* Read the stdout of a shell pipeline into a NULL-terminated array of
 * malloc'd strings. Used only for fixed, non-user-controlled pipelines
 * (listing installed packages, orphan detection, etc). */
static char **capture_lines(const char *shell_cmd, int *out_n) {
    FILE *fp = popen(shell_cmd, "r");
    char **lines = NULL;
    int n = 0, cap = 0;
    if (!fp) { *out_n = 0; return NULL; }
    char *line = NULL; size_t linecap = 0; ssize_t len;
    while ((len = getline(&line, &linecap, fp)) != -1) {
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; lines = realloc(lines, cap * sizeof(char*)); }
        lines[n++] = strdup(line);
    }
    free(line);
    pclose(fp);
    *out_n = n;
    return lines;
}

static void free_lines(char **lines, int n) {
    if (!lines) return;
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

static int ask_yes_no(const char *prompt) {
    char buf[64];
    printf("%s ", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}

/* ---------------------------------------------------------------------
 * Backend detection helpers
 * --------------------------------------------------------------------- */

static int have_flatpak(void) { return have_cmd("flatpak"); }
static int have_snap(void)    { return have_cmd("snap"); }
static int have_nix(void)     { return have_cmd("nix-env") || have_cmd("nix"); }

static const char *lowlevel_of(const char *pm) {
    if (!strcmp(pm, "apt")) return "dpkg";
    if (!strcmp(pm, "dnf")) return "rpm";
    if (!strcmp(pm, "zypper")) return "rpm";
    if (!strcmp(pm, "pacman")) return "aur";
    if (!strcmp(pm, "slackpkg")) return "pkgtool";
    return "";
}

static int detect_package_manager(void) {
    const char *candidates[][2] = {
        {"apt-get", "apt"}, {"dnf", "dnf"}, {"zypper", "zypper"},
        {"pacman", "pacman"}, {"slackpkg", "slackpkg"},
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        if (have_cmd(candidates[i][0])) {
            strncpy(PACKAGE_MANAGER, candidates[i][1], sizeof PACKAGE_MANAGER - 1);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * Path shortening -- same style as the shorten_path() bash function this
 * project's prompt was originally modeled on, so the cpkgsh prompt still
 * looks like the rest of your terminal. cpkgsh/interactive build only.
 * --------------------------------------------------------------------- */

#ifndef CPKG_BUILD
static void shorten_path(char *out, size_t outsz) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) { snprintf(out, outsz, "?"); return; }

    char *slash = strrchr(cwd, '/');
    if (!slash) { snprintf(out, outsz, "%s", cwd); return; }

    size_t dirlen = (size_t)(slash - cwd);
    const char *last = slash + 1;

    if (dirlen == 0) { /* dir is empty (cwd == "/" or similar) */
        snprintf(out, outsz, "%s", cwd);
        return;
    }

    char dir[PATH_MAX];
    memcpy(dir, cwd, dirlen);
    dir[dirlen] = '\0';

    char result[PATH_MAX] = "";
    size_t start = 0, len = dirlen;
    for (size_t k = 0; k <= len; k++) {
        if (k == len || dir[k] == '/') {
            size_t fieldlen = k - start;
            if (fieldlen == 0) {
                strncat(result, "/", sizeof(result) - strlen(result) - 1);
            } else {
                char piece[3] = { dir[start], '/', '\0' };
                strncat(result, piece, sizeof(result) - strlen(result) - 1);
            }
            start = k + 1;
        }
    }
    size_t rl = strlen(result);
    if (rl > 0 && result[rl - 1] == '/') result[rl - 1] = '\0';

    snprintf(out, outsz, "%s/%s", result, last);
}
#endif /* !CPKG_BUILD */

/* ---------------------------------------------------------------------
 * Prompt (cpkgsh/interactive only)
 * --------------------------------------------------------------------- */

#ifndef CPKG_BUILD
static void build_prompt(char *out, size_t outsz) {
    const char *low = lowlevel_of(PACKAGE_MANAGER);
    char label[64];
    const char *color;
    char upper_pm[16];
    size_t i;
    for (i = 0; PACKAGE_MANAGER[i] && i < sizeof upper_pm - 1; i++)
        upper_pm[i] = toupper((unsigned char)PACKAGE_MANAGER[i]);
    upper_pm[i] = '\0';

    if (!strcmp(PKG_MODE, "native")) {
        snprintf(label, sizeof label, "%s/%s", upper_pm, low);
        if (!strcmp(PACKAGE_MANAGER, "apt")) color = "\033[1;31m";
        else if (!strcmp(PACKAGE_MANAGER, "dnf")) color = "\033[1;94m";
        else if (!strcmp(PACKAGE_MANAGER, "zypper")) color = "\033[1;32m";
        else if (!strcmp(PACKAGE_MANAGER, "pacman")) color = "\033[1;34m";
        else color = "\033[1;95m"; /* slackpkg */
    } else if (!strcmp(PKG_MODE, "aur")) {
        snprintf(label, sizeof label, "%s/AUR", PACKAGE_MANAGER);
        color = "\033[1;97;41m";
    } else if (!strcmp(PKG_MODE, "dpkg") || !strcmp(PKG_MODE, "rpm") || !strcmp(PKG_MODE, "pkgtool")) {
        char upper_mode[16];
        for (i = 0; PKG_MODE[i] && i < sizeof upper_mode - 1; i++)
            upper_mode[i] = toupper((unsigned char)PKG_MODE[i]);
        upper_mode[i] = '\0';
        snprintf(label, sizeof label, "%s/%s", PACKAGE_MANAGER, upper_mode);
        color = "\033[1;35m";
    } else if (!strcmp(PKG_MODE, "flatpak")) {
        snprintf(label, sizeof label, "flatpak");
        color = "\033[1;36m";
    } else if (!strcmp(PKG_MODE, "snap")) {
        snprintf(label, sizeof label, "snap");
        color = "\033[1;33m";
    } else { /* nix */
        snprintf(label, sizeof label, "nix");
        color = "\033[1;96m";
    }

    char path[PATH_MAX];
    shorten_path(path, sizeof path);

    /* Same green (1;32) as GREEN in .bashrc's PS1 around shorten_path.
     * Raw ANSI is used (not readline's \001/\002 wrappers) to match the
     * original bash-side prompt's own approach exactly. */
    snprintf(out, outsz, "%s(%s)\033[00m \033[1;32m%s\033[00m\xC2\xA7 ",
             color, label, path);
}
#endif /* !CPKG_BUILD */

/* ---------------------------------------------------------------------
 * install_via -- install a set of packages through a named backend.
 * Mirrors _pkg_install_via in the original bash module. Returns 0 if it
 * believes at least the attempt succeeded.
 * --------------------------------------------------------------------- */

static int install_via(const char *mgr, char **pkgs, int npkgs) {
    if (npkgs == 0) return 0;
    if (!strcmp(mgr, "apt"))     return RUN_PREFIXED(pkgs, npkgs, "sudo","apt","install","-y") == 0;
    if (!strcmp(mgr, "dnf"))     return RUN_PREFIXED(pkgs, npkgs, "sudo","dnf","install","-y") == 0;
    if (!strcmp(mgr, "zypper"))  return RUN_PREFIXED(pkgs, npkgs, "sudo","zypper","--non-interactive","install") == 0;
    if (!strcmp(mgr, "pacman"))  return RUN_PREFIXED(pkgs, npkgs, "sudo","pacman","-S","--noconfirm") == 0;
    if (!strcmp(mgr, "slackpkg")) return RUN_PREFIXED(pkgs, npkgs, "sudo","slackpkg","install") == 0;
    if (!strcmp(mgr, "flatpak")) return RUN_PREFIXED(pkgs, npkgs, "flatpak","install","-y") == 0;
    if (!strcmp(mgr, "snap")) {
        int ok = 1;
        for (int i = 0; i < npkgs; i++) {
            char *argv[] = {"sudo","snap","install", pkgs[i], NULL};
            if (run_argv(argv) != 0) ok = 0;
        }
        return ok;
    }
    if (!strcmp(mgr, "nix")) {
        int any_ok = 0;
        for (int i = 0; i < npkgs; i++) {
            char attr[512];
            snprintf(attr, sizeof attr, "nixpkgs.%s", pkgs[i]);
            char *argv[] = {"nix-env","-iA", attr, NULL};
            if (run_argv(argv) == 0) any_ok = 1;
        }
        return any_ok;
    }
    fprintf(stderr, PROG_NAME ": don't know how to install via '%s'.\n", mgr);
    return 0;
}

/* ---------------------------------------------------------------------
 * pkg_run -- the main verb dispatcher. Mirrors _pkg_run in the bash
 * module: handles the universal modes first (flatpak/snap/nix), then
 * falls through to PACKAGE_MANAGER x PKG_MODE x action for the rest.
 * --------------------------------------------------------------------- */

static int pkg_run(const char *action, char **args, int nargs) {
    if (!strcmp(PKG_MODE, "flatpak")) {
        if (!strcmp(action,"install"))    return RUN_PREFIXED(args,nargs,"flatpak","install");
        if (!strcmp(action,"remove"))     return RUN_PREFIXED(args,nargs,"flatpak","uninstall");
        if (!strcmp(action,"purge"))      return RUN_PREFIXED(args,nargs,"flatpak","uninstall","--delete-data");
        if (!strcmp(action,"update"))     return RUN_PREFIXED(NULL,0,"flatpak","update","--appstream");
        if (!strcmp(action,"upgrade"))    return RUN_PREFIXED(args,nargs,"flatpak","update");
        if (!strcmp(action,"search"))     return RUN_PREFIXED(args,nargs,"flatpak","search");
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"flatpak","info");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"flatpak","list");
        if (!strcmp(action,"autoremove") || !strcmp(action,"clean"))
                                          return RUN_PREFIXED(NULL,0,"flatpak","uninstall","--unused");
        printf(PROG_NAME ": '%s' isn't available in flatpak mode.\n", action);
        return 0;
    }
    if (!strcmp(PKG_MODE, "snap")) {
        if (!strcmp(action,"install"))    return RUN_PREFIXED(args,nargs,"sudo","snap","install");
        if (!strcmp(action,"remove"))     return RUN_PREFIXED(args,nargs,"sudo","snap","remove");
        if (!strcmp(action,"purge"))      return RUN_PREFIXED(args,nargs,"sudo","snap","remove","--purge");
        if (!strcmp(action,"update"))     return RUN_PREFIXED(NULL,0,"sudo","snap","refresh","--list");
        if (!strcmp(action,"upgrade"))    return RUN_PREFIXED(args,nargs,"sudo","snap","refresh");
        if (!strcmp(action,"search"))     return RUN_PREFIXED(args,nargs,"snap","find");
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"snap","info");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"snap","list");
        if (!strcmp(action,"autoremove")) { printf("Not applicable in snap mode.\n"); return 0; }
        if (!strcmp(action,"clean"))      return RUN_PREFIXED(NULL,0,"sudo","snap","set","system","refresh.retain=2");
        printf(PROG_NAME ": '%s' isn't available in snap mode.\n", action);
        return 0;
    }
    if (!strcmp(PKG_MODE, "nix")) {
        if (!strcmp(action,"install")) {
            int ok = install_via("nix", args, nargs);
            return ok ? 0 : 1;
        }
        if (!strcmp(action,"remove") || !strcmp(action,"purge"))
                                          return RUN_PREFIXED(args,nargs,"nix-env","-e");
        if (!strcmp(action,"update"))     return RUN_PREFIXED(NULL,0,"nix-channel","--update");
        if (!strcmp(action,"upgrade"))    return RUN_PREFIXED(args,nargs,"nix-env","-u");
        if (!strcmp(action,"search"))     return RUN_PREFIXED(args,nargs,"nix","search","nixpkgs");
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"nix-env","-qa","--description");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"nix-env","-q");
        if (!strcmp(action,"autoremove") || !strcmp(action,"clean"))
                                          return RUN_PREFIXED(NULL,0,"nix-collect-garbage","-d");
        printf(PROG_NAME ": '%s' isn't available in nix mode.\n", action);
        return 0;
    }

    const char *pm = PACKAGE_MANAGER, *md = PKG_MODE;

    /* ---- apt / dpkg ---- */
    if (!strcmp(pm,"apt") && !strcmp(md,"native")) {
        if (!strcmp(action,"install"))    return RUN_PREFIXED(args,nargs,"sudo","apt","install");
        if (!strcmp(action,"remove"))     return RUN_PREFIXED(args,nargs,"sudo","apt","remove");
        if (!strcmp(action,"purge"))      return RUN_PREFIXED(args,nargs,"sudo","apt","purge");
        if (!strcmp(action,"update"))     return RUN_PREFIXED(NULL,0,"sudo","apt","update");
        if (!strcmp(action,"upgrade"))    return RUN_PREFIXED(args,nargs,"sudo","apt","upgrade");
        if (!strcmp(action,"search"))     return RUN_PREFIXED(args,nargs,"apt","search");
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"apt","show");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"apt","list","--installed");
        if (!strcmp(action,"autoremove")) return RUN_PREFIXED(NULL,0,"sudo","apt","autoremove");
        if (!strcmp(action,"clean"))      return RUN_PREFIXED(NULL,0,"sudo","apt","clean");
    }
    if (!strcmp(pm,"apt") && !strcmp(md,"dpkg")) {
        if (!strcmp(action,"install")) {
            if (nargs == 0) { printf("dpkg installs local .deb files, not repo packages. Usage: install /path/to/file.deb\n"); return 0; }
            return RUN_PREFIXED(args,nargs,"sudo","dpkg","-i");
        }
        if (!strcmp(action,"remove"))     return RUN_PREFIXED(args,nargs,"sudo","dpkg","-r");
        if (!strcmp(action,"purge"))      return RUN_PREFIXED(args,nargs,"sudo","dpkg","-P");
        if (!strcmp(action,"update") || !strcmp(action,"upgrade")) {
            printf("dpkg has no repository concept -- update/upgrade aren't applicable. Use 'mode apt' for that.\n");
            return 0;
        }
        if (!strcmp(action,"search")) {
            char cmd[512] = "dpkg -l | grep -i -- \"";
            for (int i=0;i<nargs;i++){ strncat(cmd,args[i],sizeof(cmd)-strlen(cmd)-2); strncat(cmd," ",sizeof(cmd)-strlen(cmd)-2);}
            strncat(cmd,"\"",sizeof(cmd)-strlen(cmd)-1);
            return system(cmd);
        }
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"dpkg","-s");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"dpkg","-l");
        if (!strcmp(action,"autoremove") || !strcmp(action,"clean")) { printf("Not applicable in dpkg mode.\n"); return 0; }
    }

    /* ---- dnf/zypper (rpm-based) ---- */
    if ((!strcmp(pm,"dnf")||!strcmp(pm,"zypper")) && !strcmp(md,"native")) {
        const char *tool = !strcmp(pm,"dnf") ? "dnf" : "zypper";
        if (!strcmp(action,"install"))    return RUN_PREFIXED(args,nargs,"sudo",(char*)tool,"install");
        if (!strcmp(action,"remove"))     return RUN_PREFIXED(args,nargs,"sudo",(char*)tool,"remove");
        if (!strcmp(action,"purge")) {
            if (!strcmp(pm,"dnf")) return RUN_PREFIXED(args,nargs,"sudo","dnf","remove");
            return RUN_PREFIXED(args,nargs,"sudo","zypper","remove","--clean-deps");
        }
        if (!strcmp(action,"update")) {
            if (!strcmp(pm,"dnf")) return RUN_PREFIXED(NULL,0,"sudo","dnf","check-update");
            return RUN_PREFIXED(NULL,0,"sudo","zypper","refresh");
        }
        if (!strcmp(action,"upgrade")) {
            if (!strcmp(pm,"dnf")) return RUN_PREFIXED(args,nargs,"sudo","dnf","upgrade");
            return RUN_PREFIXED(args,nargs,"sudo","zypper","update");
        }
        if (!strcmp(action,"search"))     return RUN_PREFIXED(args,nargs,(char*)tool,"search");
        if (!strcmp(action,"show")) {
            if (!strcmp(pm,"dnf")) return RUN_PREFIXED(args,nargs,"dnf","info");
            return RUN_PREFIXED(args,nargs,"zypper","info");
        }
        if (!strcmp(action,"list")) {
            if (!strcmp(pm,"dnf")) return RUN_PREFIXED(NULL,0,"dnf","list","installed");
            return RUN_PREFIXED(NULL,0,"zypper","packages","--installed-only");
        }
        if (!strcmp(action,"autoremove")) {
            if (!strcmp(pm,"dnf")) return RUN_PREFIXED(NULL,0,"sudo","dnf","autoremove");
            int n; char **lines = capture_lines(
                "zypper --no-refresh packages --unneeded 2>/dev/null | awk -F'|' '/^i/{gsub(/ /,\"\",$3); print $3}'",
                &n);
            if (n == 0) { printf("No unneeded packages found.\n"); free_lines(lines,n); return 0; }
            int rc = RUN_PREFIXED(lines,n,"sudo","zypper","remove");
            free_lines(lines,n);
            return rc;
        }
        if (!strcmp(action,"clean")) {
            if (!strcmp(pm,"dnf")) return RUN_PREFIXED(NULL,0,"sudo","dnf","clean","all");
            return RUN_PREFIXED(NULL,0,"sudo","zypper","clean");
        }
    }
    if ((!strcmp(pm,"dnf")||!strcmp(pm,"zypper")) && !strcmp(md,"rpm")) {
        if (!strcmp(action,"install")) {
            if (nargs == 0) { printf("rpm installs local .rpm files, not repo packages. Usage: install /path/to/file.rpm\n"); return 0; }
            return RUN_PREFIXED(args,nargs,"sudo","rpm","-i");
        }
        if (!strcmp(action,"remove") || !strcmp(action,"purge"))
                                          return RUN_PREFIXED(args,nargs,"sudo","rpm","-e");
        if (!strcmp(action,"update")) {
            if (nargs == 0) { printf("Usage: update /path/to/file.rpm (rpm has no repo index to refresh)\n"); return 0; }
            return RUN_PREFIXED(args,nargs,"sudo","rpm","-U");
        }
        if (!strcmp(action,"upgrade")) {
            if (nargs == 0) { printf("Usage: upgrade /path/to/file.rpm\n"); return 0; }
            return RUN_PREFIXED(args,nargs,"sudo","rpm","-U");
        }
        if (!strcmp(action,"search")) {
            char cmd[512] = "rpm -qa | grep -i -- \"";
            for (int i=0;i<nargs;i++){ strncat(cmd,args[i],sizeof(cmd)-strlen(cmd)-2); strncat(cmd," ",sizeof(cmd)-strlen(cmd)-2);}
            strncat(cmd,"\"",sizeof(cmd)-strlen(cmd)-1);
            return system(cmd);
        }
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"rpm","-qi");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"rpm","-qa");
        if (!strcmp(action,"autoremove") || !strcmp(action,"clean")) { printf("Not applicable in rpm mode.\n"); return 0; }
    }

    /* ---- pacman / aur ---- */
    if (!strcmp(pm,"pacman") && !strcmp(md,"native")) {
        if (!strcmp(action,"install"))    return RUN_PREFIXED(args,nargs,"sudo","pacman","-S");
        if (!strcmp(action,"remove"))     return RUN_PREFIXED(args,nargs,"sudo","pacman","-R");
        if (!strcmp(action,"purge"))      return RUN_PREFIXED(args,nargs,"sudo","pacman","-Rns");
        if (!strcmp(action,"update"))     return RUN_PREFIXED(NULL,0,"sudo","pacman","-Sy");
        if (!strcmp(action,"upgrade"))    return RUN_PREFIXED(args,nargs,"sudo","pacman","-Syu");
        if (!strcmp(action,"search"))     return RUN_PREFIXED(args,nargs,"pacman","-Ss");
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"pacman","-Si");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"pacman","-Q");
        if (!strcmp(action,"autoremove")) {
            int n; char **lines = capture_lines("pacman -Qtdq 2>/dev/null", &n);
            if (n == 0) { printf("No orphaned packages found.\n"); free_lines(lines,n); return 0; }
            int rc = RUN_PREFIXED(lines,n,"sudo","pacman","-Rns");
            free_lines(lines,n);
            return rc;
        }
        if (!strcmp(action,"clean"))      return RUN_PREFIXED(NULL,0,"sudo","pacman","-Sc");
    }
    if (!strcmp(pm,"pacman") && !strcmp(md,"aur")) {
        if (!strcmp(action,"install"))    return RUN_PREFIXED(args,nargs,"yay","-S");
        if (!strcmp(action,"remove"))     return RUN_PREFIXED(args,nargs,"yay","-R");
        if (!strcmp(action,"purge"))      return RUN_PREFIXED(args,nargs,"yay","-Rns");
        if (!strcmp(action,"update"))     return RUN_PREFIXED(NULL,0,"yay","-Sy");
        if (!strcmp(action,"upgrade"))    return RUN_PREFIXED(args,nargs,"yay","-Syu");
        if (!strcmp(action,"search"))     return RUN_PREFIXED(args,nargs,"yay","-Ss");
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"yay","-Si");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"yay","-Qm");
        if (!strcmp(action,"autoremove")) {
            int n; char **lines = capture_lines("pacman -Qtdq 2>/dev/null", &n);
            if (n == 0) { printf("No orphaned packages found.\n"); free_lines(lines,n); return 0; }
            int rc = RUN_PREFIXED(lines,n,"sudo","pacman","-Rns");
            free_lines(lines,n);
            return rc;
        }
        if (!strcmp(action,"clean"))      return RUN_PREFIXED(NULL,0,"yay","-Sc");
    }

    /* ---- slackpkg / pkgtool ---- */
    if (!strcmp(pm,"slackpkg") && !strcmp(md,"native")) {
        if (!strcmp(action,"install"))    return RUN_PREFIXED(args,nargs,"sudo","slackpkg","install");
        if (!strcmp(action,"remove"))     return RUN_PREFIXED(args,nargs,"sudo","slackpkg","remove");
        if (!strcmp(action,"purge"))      return RUN_PREFIXED(args,nargs,"sudo","slackpkg","remove");
        if (!strcmp(action,"update"))     return RUN_PREFIXED(NULL,0,"sudo","slackpkg","update");
        if (!strcmp(action,"upgrade")) {
            if (nargs == 0) return RUN_PREFIXED(NULL,0,"sudo","slackpkg","upgrade-all");
            return RUN_PREFIXED(args,nargs,"sudo","slackpkg","upgrade");
        }
        if (!strcmp(action,"search"))     return RUN_PREFIXED(args,nargs,"slackpkg","search");
        if (!strcmp(action,"show"))       return RUN_PREFIXED(args,nargs,"slackpkg","info");
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"ls","/var/log/packages");
        if (!strcmp(action,"autoremove")) { printf("Slackware doesn't track dependencies -- nothing to autoremove.\n"); return 0; }
        if (!strcmp(action,"clean"))      return RUN_PREFIXED(NULL,0,"sudo","slackpkg","clean-system");
    }
    if (!strcmp(pm,"slackpkg") && !strcmp(md,"pkgtool")) {
        if (!strcmp(action,"install")) {
            if (nargs == 0) { printf("pkgtool installs local .t?z package files, not repo packages. Usage: install /path/to/pkg.txz\n"); return 0; }
            return RUN_PREFIXED(args,nargs,"sudo","installpkg");
        }
        if (!strcmp(action,"remove") || !strcmp(action,"purge"))
                                          return RUN_PREFIXED(args,nargs,"sudo","removepkg");
        if (!strcmp(action,"update") || !strcmp(action,"upgrade")) {
            if (nargs == 0) { printf("pkgtool has no repository concept -- update/upgrade aren't applicable. Use 'mode slackpkg' for that.\n"); return 0; }
            return RUN_PREFIXED(args,nargs,"sudo","upgradepkg");
        }
        if (!strcmp(action,"search")) {
            char cmd[512] = "ls /var/log/packages | grep -i -- \"";
            for (int i=0;i<nargs;i++){ strncat(cmd,args[i],sizeof(cmd)-strlen(cmd)-2); strncat(cmd," ",sizeof(cmd)-strlen(cmd)-2);}
            strncat(cmd,"\"",sizeof(cmd)-strlen(cmd)-1);
            return system(cmd);
        }
        if (!strcmp(action,"show")) {
            if (nargs == 0) { printf("Usage: show <package>\n"); return 0; }
            char path[512];
            snprintf(path, sizeof path, "/var/log/packages/%s", args[0]);
            FILE *f = fopen(path, "r");
            if (!f) { printf("pkgtool: no such installed package '%s'.\n", args[0]); return 0; }
            char buf[4096]; size_t r;
            while ((r = fread(buf,1,sizeof buf,f)) > 0) fwrite(buf,1,r,stdout);
            fclose(f);
            return 0;
        }
        if (!strcmp(action,"list"))       return RUN_PREFIXED(NULL,0,"ls","/var/log/packages");
        if (!strcmp(action,"autoremove") || !strcmp(action,"clean")) { printf("Not applicable in pkgtool mode.\n"); return 0; }
    }

    printf(PROG_NAME ": '%s' isn't available in %s/%s mode.\n", action, pm, md);
    return 0;
}

/* ---------------------------------------------------------------------
 * mode command
 * --------------------------------------------------------------------- */

static void mode_cmd(const char *arg_raw) {
    char arg[32] = "";
    if (arg_raw) {
        size_t i;
        for (i = 0; arg_raw[i] && i < sizeof arg - 1; i++) arg[i] = tolower((unsigned char)arg_raw[i]);
        arg[i] = '\0';
    }

    if (arg[0] == '\0') {
        if (!strcmp(PKG_MODE,"native")) { mode_cmd(lowlevel_of(PACKAGE_MANAGER)); return; }
        if (!strcmp(PKG_MODE,"dpkg")||!strcmp(PKG_MODE,"rpm")||!strcmp(PKG_MODE,"aur")||!strcmp(PKG_MODE,"pkgtool")) {
            mode_cmd(PACKAGE_MANAGER); return;
        }
        printf("mode: no low-level counterpart for '%s' -- switching back to native %s.\n", PKG_MODE, PACKAGE_MANAGER);
        char up[16]; size_t i; for(i=0;PACKAGE_MANAGER[i]&&i<sizeof up-1;i++) up[i]=toupper((unsigned char)PACKAGE_MANAGER[i]); up[i]=0;
        mode_cmd(PACKAGE_MANAGER);
        return;
    }

    if (!strcmp(arg,"apt")||!strcmp(arg,"dnf")||!strcmp(arg,"zypper")||!strcmp(arg,"pacman")||!strcmp(arg,"slackpkg")) {
        if (!strcmp(arg, PACKAGE_MANAGER)) {
            strcpy(PKG_MODE, "native");
            printf("Switched to native %s mode.\n", PACKAGE_MANAGER);
        } else {
            printf("mode: '%s' isn't the active backend (you're using %s).\n", arg_raw, PACKAGE_MANAGER);
        }
        return;
    }
    if (!strcmp(arg,"dpkg")) {
        if (!strcmp(PACKAGE_MANAGER,"apt")) { strcpy(PKG_MODE,"dpkg"); printf("Switched to DPKG (low-level) mode.\n"); }
        else printf("mode: dpkg mode is only available under APT.\n");
        return;
    }
    if (!strcmp(arg,"rpm")) {
        if (!strcmp(PACKAGE_MANAGER,"dnf")||!strcmp(PACKAGE_MANAGER,"zypper")) { strcpy(PKG_MODE,"rpm"); printf("Switched to RPM (low-level) mode.\n"); }
        else printf("mode: rpm mode is only available under DNF or ZYPPER.\n");
        return;
    }
    if (!strcmp(arg,"pkgtool")) {
        if (!strcmp(PACKAGE_MANAGER,"slackpkg")) { strcpy(PKG_MODE,"pkgtool"); printf("Switched to pkgtool (low-level) mode.\n"); }
        else printf("mode: pkgtool mode is only available under SLACKPKG.\n");
        return;
    }
    if (!strcmp(arg,"aur")) {
        if (strcmp(PACKAGE_MANAGER,"pacman")) { printf("mode: aur mode is only available under PACMAN.\n"); return; }
        if (!have_cmd("yay")) { printf("mode: yay is not installed -- AUR mode is unavailable. Install yay first.\n"); return; }
        printf("\033[1;97;41m WARNING \033[0m AUR packages are user-submitted and NOT reviewed by Arch maintainers.\n");
        printf("\033[1;97;41m WARNING \033[0m They can contain malicious code -- only install what you trust.\n");
        strcpy(PKG_MODE,"aur");
        printf("Switched to AUR (low-level) mode via yay.\n");
        return;
    }
    if (!strcmp(arg,"flatpak")) {
        if (!have_flatpak()) { printf("mode: flatpak is not installed -- mode unavailable.\n"); return; }
        strcpy(PKG_MODE,"flatpak"); printf("Switched to flatpak mode.\n"); return;
    }
    if (!strcmp(arg,"snap")) {
        if (!have_snap()) { printf("mode: snap is not installed -- mode unavailable.\n"); return; }
        strcpy(PKG_MODE,"snap"); printf("Switched to snap mode.\n"); return;
    }
    if (!strcmp(arg,"nix")) {
        if (!have_nix()) { printf("mode: nix is not installed -- mode unavailable.\n"); return; }
        strcpy(PKG_MODE,"nix"); printf("Switched to nix mode.\n"); return;
    }
    printf("mode: unknown mode '%s'. Type 'help' for options.\n", arg_raw ? arg_raw : "");
}

/* ---------------------------------------------------------------------
 * install-failure fallback: offer other managers, same as
 * _pkg_offer_alternates in the bash version.
 * --------------------------------------------------------------------- */

static void offer_alternates(char **pkgs, int npkgs) {
    const char *current = (!strcmp(PKG_MODE,"flatpak")||!strcmp(PKG_MODE,"snap")||!strcmp(PKG_MODE,"nix"))
                          ? PKG_MODE : PACKAGE_MANAGER;
    const char *alts[8]; int n = 0;
    if (strcmp(current, PACKAGE_MANAGER)) alts[n++] = PACKAGE_MANAGER;
    if (strcmp(current, "flatpak") && have_flatpak()) alts[n++] = "flatpak";
    if (strcmp(current, "snap") && have_snap()) alts[n++] = "snap";
    if (strcmp(current, "nix") && have_nix()) alts[n++] = "nix";
    if (n == 0) return;

    printf("Try to install from other package managers?\n");
    for (int i = 0; i < n; i++) {
        char up[16]; size_t j; for (j=0; alts[i][j] && j<sizeof up-1; j++) up[j]=toupper((unsigned char)alts[i][j]); up[j]=0;
        printf("  %d) %s\n", i+1, up);
    }
    printf("  0) No\n");
    printf("Choose: ");
    fflush(stdout);
    char buf[16];
    if (!fgets(buf, sizeof buf, stdin)) return;
    int choice = atoi(buf);
    if (choice <= 0 || choice > n) { printf("Cancelled.\n"); return; }
    const char *chosen = alts[choice-1];
    char upc[16]; size_t j; for (j=0; chosen[j] && j<sizeof upc-1; j++) upc[j]=toupper((unsigned char)chosen[j]); upc[j]=0;
    printf("Trying install via %s...\n", upc);
    install_via(chosen, pkgs, npkgs);
}

/* ---------------------------------------------------------------------
 * config generate / load
 * --------------------------------------------------------------------- */

static char **native_installed_list(int *n) {
    const char *cmd = NULL;
    if (!strcmp(PACKAGE_MANAGER,"apt"))      cmd = "apt-mark showmanual 2>/dev/null";
    else if (!strcmp(PACKAGE_MANAGER,"dnf")) cmd = "dnf repoquery --userinstalled --qf '%{name}' 2>/dev/null || rpm -qa --qf '%{NAME}\\n' 2>/dev/null";
    else if (!strcmp(PACKAGE_MANAGER,"zypper")) cmd = "rpm -qa --qf '%{NAME}\\n' 2>/dev/null";
    else if (!strcmp(PACKAGE_MANAGER,"pacman")) cmd = "pacman -Qqe 2>/dev/null";
    else if (!strcmp(PACKAGE_MANAGER,"slackpkg")) cmd = "ls /var/log/packages 2>/dev/null | rev | cut -d- -f4- | rev";
    if (!cmd) { *n = 0; return NULL; }
    return capture_lines(cmd, n);
}

static void config_generate(const char *file_arg) {
    char file[512];
    if (file_arg && file_arg[0]) snprintf(file, sizeof file, "%s", file_arg);
    else {
        time_t t = time(NULL);
        struct tm tmv; localtime_r(&t, &tmv);
        snprintf(file, sizeof file, PROG_NAME "-config-%04d%02d%02d.txt", tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday);
    }

    FILE *f = fopen(file, "w");
    if (!f) { printf(PROG_NAME ": couldn't open %s for writing.\n", file); return; }

    time_t t = time(NULL);
    char tbuf[64]; strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", localtime(&t));
    fprintf(f, "# cpkgsh config\n# generated %s\n# native package manager on the generating system: %s\n",
            tbuf, PACKAGE_MANAGER);

    int count = 0, n;
    char **lines = native_installed_list(&n);
    for (int i = 0; i < n; i++) { fprintf(f, "NATIVE %s\n", lines[i]); count++; }
    free_lines(lines, n);

    if (have_flatpak()) {
        lines = capture_lines("flatpak list --app --columns=application 2>/dev/null", &n);
        for (int i = 0; i < n; i++) { fprintf(f, "FLATPAK %s\n", lines[i]); count++; }
        free_lines(lines, n);
    }
    if (have_snap()) {
        lines = capture_lines("snap list 2>/dev/null | awk 'NR>1{print $1}'", &n);
        for (int i = 0; i < n; i++) { fprintf(f, "SNAP %s\n", lines[i]); count++; }
        free_lines(lines, n);
    }
    if (have_nix()) {
        lines = capture_lines("nix-env -q 2>/dev/null", &n);
        for (int i = 0; i < n; i++) { fprintf(f, "NIX %s\n", lines[i]); count++; }
        free_lines(lines, n);
    }

    fclose(f);
    printf("Config written to %s (%d package(s)).\n", file, count);
}

static int setup_flatpak(void) {
    if (have_flatpak()) return 1;
    if (!ask_yes_no("Flatpak isn't installed but the config needs it. Install it now? [y/N]")) {
        printf("Skipping flatpak packages.\n"); return 0;
    }
    int rc = 1;
    if (!strcmp(PACKAGE_MANAGER,"apt"))      rc = run_argv((char*[]){"sudo","apt","install","-y","flatpak",NULL});
    else if (!strcmp(PACKAGE_MANAGER,"dnf")) rc = run_argv((char*[]){"sudo","dnf","install","-y","flatpak",NULL});
    else if (!strcmp(PACKAGE_MANAGER,"zypper")) rc = run_argv((char*[]){"sudo","zypper","install","-y","flatpak",NULL});
    else if (!strcmp(PACKAGE_MANAGER,"pacman")) rc = run_argv((char*[]){"sudo","pacman","-S","--noconfirm","flatpak",NULL});
    else { printf(PROG_NAME ": no known automatic Flatpak install path for Slackware -- install it manually.\n"); return 0; }
    (void)rc;
    if (system("flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo 2>/dev/null")) {}
    return have_flatpak();
}

static int setup_snap(void) {
    if (have_snap()) return 1;
    if (!ask_yes_no("Snap isn't installed but the config needs it. Install it now? [y/N]")) {
        printf("Skipping snap packages.\n"); return 0;
    }
    if (!strcmp(PACKAGE_MANAGER,"apt"))      run_argv((char*[]){"sudo","apt","install","-y","snapd",NULL});
    else if (!strcmp(PACKAGE_MANAGER,"dnf")) run_argv((char*[]){"sudo","dnf","install","-y","snapd",NULL});
    else if (!strcmp(PACKAGE_MANAGER,"zypper")) run_argv((char*[]){"sudo","zypper","install","-y","snapd",NULL});
    else if (!strcmp(PACKAGE_MANAGER,"pacman")) { printf(PROG_NAME ": snapd isn't in the official Arch repos -- install it from the AUR first (mode aur).\n"); return 0; }
    else { printf(PROG_NAME ": no known automatic snapd install path for Slackware -- install it manually.\n"); return 0; }
    return have_snap();
}

static int setup_nix(void) {
    if (have_nix()) return 1;
    if (!ask_yes_no("Nix isn't installed but the config needs it. Run the official Nix installer now? [y/N]")) {
        printf("Skipping nix packages.\n"); return 0;
    }
    if (system("sh <(curl -L https://nixos.org/nix/install) --daemon")) {}
    return have_nix();
}

static void config_load(const char *file) {
    if (!file || !file[0]) { printf(PROG_NAME ": usage: config load <file>\n"); return; }
    FILE *f = fopen(file, "r");
    if (!f) { printf(PROG_NAME ": usage: config load <file>\n"); return; }

    char *native[512], *flatpak[512], *snap_p[512], *nix_p[512];
    int nn=0, nf=0, ns=0, nx=0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l>0 && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]='\0';
        if (l == 0 || line[0] == '#') continue;
        char tag[32], name[900];
        if (sscanf(line, "%31s %899[^\n]", tag, name) != 2) continue;
        char *dup = strdup(name);
        if (!strcmp(tag,"NATIVE") && nn<512) native[nn++]=dup;
        else if (!strcmp(tag,"FLATPAK") && nf<512) flatpak[nf++]=dup;
        else if (!strcmp(tag,"SNAP") && ns<512) snap_p[ns++]=dup;
        else if (!strcmp(tag,"NIX") && nx<512) nix_p[nx++]=dup;
        else free(dup);
    }
    fclose(f);

    printf("Config: %d native, %d flatpak, %d snap, %d nix package(s).\n", nn, nf, ns, nx);

    if (nf > 0 && !setup_flatpak()) nf = 0;
    if (ns > 0 && !setup_snap())    ns = 0;
    if (nx > 0 && !setup_nix())     nx = 0;

    char *failed[512]; int nfailed = 0;
    for (int i = 0; i < nn; i++) {
        printf("Installing (native/%s): %s\n", PACKAGE_MANAGER, native[i]);
        if (!install_via(PACKAGE_MANAGER, &native[i], 1)) failed[nfailed++] = native[i];
    }

    if (nfailed > 0) {
        printf("Not available through %s:", PACKAGE_MANAGER);
        for (int i = 0; i < nfailed; i++) printf(" %s", failed[i]);
        printf("\n");
        const char *order[3] = {"flatpak","nix","snap"};
        for (int oi = 0; oi < 3 && nfailed > 0; oi++) {
            char q[128];
            snprintf(q, sizeof q, "Search for the %d remaining package(s) on %s? [y/N]", nfailed, order[oi]);
            /* uppercase the manager name in the prompt */
            char up[16]; size_t j; for (j=0; order[oi][j] && j<sizeof up-1; j++) up[j]=toupper((unsigned char)order[oi][j]); up[j]=0;
            snprintf(q, sizeof q, "Search for the %d remaining package(s) on %s? [y/N]", nfailed, up);
            if (!ask_yes_no(q)) continue;
            int ok;
            if (!strcmp(order[oi],"flatpak")) ok = setup_flatpak();
            else if (!strcmp(order[oi],"snap")) ok = setup_snap();
            else ok = setup_nix();
            if (!ok) continue;
            char *still[512]; int nstill = 0;
            for (int i = 0; i < nfailed; i++) {
                printf("Installing (%s): %s\n", order[oi], failed[i]);
                if (!install_via(order[oi], &failed[i], 1)) still[nstill++] = failed[i];
            }
            nfailed = nstill;
            for (int i = 0; i < nstill; i++) failed[i] = still[i];
        }
        if (nfailed > 0) {
            printf("Still unavailable anywhere:");
            for (int i = 0; i < nfailed; i++) printf(" %s", failed[i]);
            printf("\n");
        }
    }

    if (nf > 0) { printf("Installing flatpak packages...\n"); install_via("flatpak", flatpak, nf); }
    if (ns > 0) { printf("Installing snap packages...\n");    install_via("snap", snap_p, ns); }
    if (nx > 0) { printf("Installing nix packages...\n");     install_via("nix", nix_p, nx); }

    for (int i=0;i<nn;i++) free(native[i]);
    for (int i=0;i<nf;i++) free(flatpak[i]);
    for (int i=0;i<ns;i++) free(snap_p[i]);
    for (int i=0;i<nx;i++) free(nix_p[i]);
}

/* ---------------------------------------------------------------------
 * help
 * --------------------------------------------------------------------- */

static void print_help(void) {
    printf(
"Unified cpkg/cpkgsh commands (same names across all backends):\n"
"  (any of these also works one-shot from your regular shell prompt --\n"
"   no need to enter cpkgsh first -- as 'cpkg <command> [args...]', e.g.\n"
"   'cpkg install vim'. Plain 'cpkgsh' with no arguments enters this shell.)\n"
"\n"
"  install <pkg>    Install a package\n"
"  remove <pkg>     Remove a package (keep config)\n"
"  purge <pkg>      Remove a package and its config\n"
"  update           Refresh the package index\n"
"  upgrade [pkg]    Upgrade all packages, or one if given\n"
"  search <term>    Search for a package\n"
"  show <pkg>       Show details about a package\n"
"  list             List installed packages\n"
"  autoremove       Remove orphaned/unneeded dependencies\n"
"  clean            Clear the local package cache\n"
"  mode [name]      Show/switch backend. No args = toggle native <-> low-level.\n"
"                    Names are case-insensitive.\n"
"  config generate [file]   Write a config listing installed packages\n"
"  config load <file>       Install packages from a generated config\n"
"  help, ?          Show this help\n"
"  exit, quit       Leave cpkgsh\n"
"\n"
"Backend pairs (native / low-level) -- switch with 'mode <name>':\n"
"  APT      <-> dpkg     (dpkg only installs/removes local .deb files, no repo)\n"
"  DNF      <-> rpm      (rpm only installs/removes local .rpm files, no repo)\n"
"  ZYPPER   <-> rpm      (same as above)\n"
"  PACMAN   <-> aur      (aur mode uses yay to build/install from the AUR)\n"
"  SLACKPKG <-> pkgtool  (pkgtool only installs/removes local .t?z files, no repo)\n"
"\n"
"Universal modes (independent of your distro's package manager, only shown\n"
"if installed) -- switch with 'mode flatpak' / 'mode snap' / 'mode nix':\n"
"  flatpak, snap, nix\n"
"\n"
"If 'install' fails in the active mode, you'll be offered a choice of any\n"
"other package managers detected on your system to retry the install with.\n"
"\n"
"'config generate' writes every installed package to a file, tagged by where\n"
"it came from: NATIVE (whatever your native manager is, so it can be\n"
"re-resolved against a different native manager on another system), or\n"
"FLATPAK/SNAP/NIX. 'config load' reads such a file, offers to set up any of\n"
"flatpak/snap/nix it references if they aren't already present, installs as\n"
"much as it can through the native manager, then offers to look for whatever\n"
"native couldn't provide via flatpak, then nix, then snap, one package at a\n"
"time and asking before each search.\n"
"\n"
"WARNING: AUR packages are community-submitted and NOT reviewed by Arch\n"
"maintainers. They can contain malicious code. Only install what you trust,\n"
"and check the PKGBUILD yourself first. AUR mode requires 'yay' and is\n"
"unavailable without it.\n"
"\n"
"Only the commands listed above run in this shell -- regular shell commands\n"
"(cd, ls, cat, rm, etc.) are disabled by design.\n"
    );
}

/* ---------------------------------------------------------------------
 * sudo handling + keepalive
 * --------------------------------------------------------------------- */

static int sudo_cached(void) {
    return system("sudo -n true >/dev/null 2>&1") == 0;
}

#ifndef CPKG_BUILD
static void keepalive_loop(void) {
    while (getppid() == MAIN_PID) {
        if (system("sudo -n true >/dev/null 2>&1")) {}
        sleep(60);
    }
    _exit(0);
}

static void start_keepalive(void) {
    pid_t pid = fork();
    if (pid == 0) { keepalive_loop(); }
    else if (pid > 0) KEEPALIVE_PID = pid;
}
#endif /* !CPKG_BUILD */

/* ---------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

/* Runs one command (as would be typed at the REPL prompt). Returns 1 if
 * the command was "exit"/"quit" (caller should stop), 0 otherwise. Shared
 * between the interactive REPL and one-shot `cpkgsh <cmd> <args...>` calls
 * from the regular shell. */
static int dispatch_command(char *cmd, char **args, int nargs) {
    if (!strcmp(cmd,"install")) {
        int rc = pkg_run("install", args, nargs);
        if (rc != 0 && nargs > 0) offer_alternates(args, nargs);
    } else if (!strcmp(cmd,"remove")||!strcmp(cmd,"purge")||!strcmp(cmd,"update")||
               !strcmp(cmd,"upgrade")||!strcmp(cmd,"search")||!strcmp(cmd,"show")||
               !strcmp(cmd,"list")||!strcmp(cmd,"autoremove")||!strcmp(cmd,"clean")) {
        pkg_run(cmd, args, nargs);
    } else if (!strcmp(cmd,"mode")) {
        mode_cmd(nargs > 0 ? args[0] : NULL);
    } else if (!strcmp(cmd,"config")) {
        if (nargs == 0) printf("config: usage: config generate [file] | config load <file>\n");
        else if (!strcmp(args[0],"generate")||!strcmp(args[0],"gen")||!strcmp(args[0],"save"))
            config_generate(nargs > 1 ? args[1] : NULL);
        else if (!strcmp(args[0],"load"))
            config_load(nargs > 1 ? args[1] : NULL);
        else
            printf("config: usage: config generate [file] | config load <file>\n");
    } else if (!strcmp(cmd,"help")||!strcmp(cmd,"?")) {
        print_help();
    } else if (!strcmp(cmd,"exit")||!strcmp(cmd,"quit")) {
        return 1;
    } else {
        printf(PROG_NAME ": unknown command '%s'. Type 'help' or '?' for available commands.\n", cmd);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    MAIN_PID = getpid();

#ifdef CPKG_BUILD
    if (argc < 2) {
        fprintf(stderr, PROG_NAME ": usage: cpkg <command> [args...] (e.g. 'cpkg install vim'). For the interactive shell, run cpkgsh.\n");
        return 1;
    }
#endif

    if (!detect_package_manager()) {
        fprintf(stderr, PROG_NAME ": no supported package manager found (apt/dnf/zypper/pacman/slackpkg)\n");
        return 1;
    }

    int needed_auth = !sudo_cached();
    char upper_pm[16]; size_t i;
    for (i = 0; PACKAGE_MANAGER[i] && i < sizeof upper_pm - 1; i++) upper_pm[i] = toupper((unsigned char)PACKAGE_MANAGER[i]);
    upper_pm[i] = '\0';

    if (needed_auth) {
        char prompt[128];
        snprintf(prompt, sizeof prompt, PROG_NAME " (%s) sudo password: ", upper_pm);
        char *sudo_argv[] = {"sudo","-p",prompt,"-v",NULL};
        if (run_argv(sudo_argv) != 0) {
            fprintf(stderr, PROG_NAME ": sudo authentication failed.\n");
            return 1;
        }
#ifndef CPKG_BUILD
        /* Only cpkgsh (interactive) has a welcome line underneath to reuse;
         * cpkg is one-shot, so there's nothing below the password prompt
         * to erase -- doing it there would eat the caller's own output. */
        printf("\033[1A\033[2K\r");
#endif
    } else {
        if (system("sudo -v >/dev/null 2>&1")) {}
    }

    strcpy(PKG_MODE, "native");

#ifdef CPKG_BUILD
    /* cpkg: run exactly one command and exit. No REPL, no keepalive
     * daemon needed for something this short-lived. */
    {
        char *args[MAXARGS]; int nargs = 0;
        for (int a = 2; a < argc && nargs < MAXARGS - 1; a++) args[nargs++] = argv[a];
        if (!strcmp(argv[1], "help") || !strcmp(argv[1], "?")) {
            print_help();
            return 0;
        }
        if (!strcmp(argv[1], "exit") || !strcmp(argv[1], "quit")) {
            return 0;
        }
        dispatch_command(argv[1], args, nargs);
        return 0;
    }
#else
    /* cpkgsh: always the interactive shell. */
    start_keepalive();

    using_history();

    char *line;
    char prompt[512];
    for (;;) {
        build_prompt(prompt, sizeof prompt);
        line = readline(prompt);
        if (!line) { printf("\n"); break; }
        if (line[0] == '\0') { free(line); continue; }
        add_history(line);

        char *args[MAXARGS]; int nargs = 0;
        char *tok = strtok(line, " \t");
        char *cmd = tok;
        if (tok) tok = strtok(NULL, " \t");
        while (tok && nargs < MAXARGS - 1) { args[nargs++] = tok; tok = strtok(NULL, " \t"); }

        if (!cmd) { free(line); continue; }

        int should_exit = dispatch_command(cmd, args, nargs);
        free(line);
        if (should_exit) break;
    }

    if (KEEPALIVE_PID > 0) {
        kill(KEEPALIVE_PID, SIGTERM);
        waitpid(KEEPALIVE_PID, NULL, WNOHANG);
    }
    return 0;
#endif
}
