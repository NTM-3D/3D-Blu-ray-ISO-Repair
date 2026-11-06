#include "repair.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *PROG = "3d_bluray_iso_repair_v" BR3D_VERSION ".exe";

static void usage_line(FILE *out) {
    fprintf(out, "usage: %s [-h] [-help] [-apply] [-undo] [-order {DB,BD}] [-chatty]\n", PROG);
    fprintf(out, "       [-no-data-check] [-show-all] input\n");
}

static void banner(void) {
    printf("3D Blu-ray ISO Repair v%s\n", BR3D_VERSION);
    printf("NTM-3D\n\n");
    fflush(stdout);
}

static void help(void) {
    usage_line(stdout);
    printf("\n");
    printf("Repair UDF allocation issues in 3D Blu-ray ISO images. Accepts a single .iso\n");
    printf("file or a folder of .iso files.\n");
    printf("\n");
    printf("positional arguments:\n");
    printf("  input                 Path to a 3D Blu-ray ISO image, or a folder containing ISOs\n");
    printf("\n");
    printf("options:\n");
    printf("  -h, -help             show this help message and exit\n");
    printf("  -apply                Write changes (default: dry run)\n");
    printf("  -undo                 Restore the original sectors of a single ISO\n");
    printf("  -order {DB,BD}        Manually override chunk order (default: auto-detected)\n");
    printf("  -chatty               Show detailed technical output (single ISO mode)\n");
    printf("  -no-data-check        Skip MPEG-TS sync-byte probes\n");
    printf("  -show-all             In folder mode, also list ISOs that have no issues\n");
}

static int is_dir(const char *path) {
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
    return (st.st_mode & _S_IFMT) == _S_IFDIR;
}

static int is_file(const char *path) {
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
    return (st.st_mode & _S_IFMT) == _S_IFREG;
}

static int parse_args(int argc, char **argv, const char **input, Options *opt, int *undo) {
    *input = NULL;
    *undo = 0;
    if (argc == 1) {
        help();
        return -1;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0) {
            help();
            return -1;
        } else if (strcmp(a, "-apply") == 0) {
            opt->apply = 1;
        } else if (strcmp(a, "-undo") == 0) {
            *undo = 1;
        } else if (strcmp(a, "-chatty") == 0) {
            opt->chatty = 1;
        } else if (strcmp(a, "-no-data-check") == 0) {
            opt->no_data_check = 1;
        } else if (strcmp(a, "-show-all") == 0) {
            opt->show_all = 1;
        } else if (strcmp(a, "-order") == 0) {
            if (i + 1 >= argc) {
                usage_line(stderr);
                fprintf(stderr, "%s: error: argument -order: expected one argument\n", PROG);
                return 2;
            }
            const char *v = argv[++i];
            if (strcmp(v, "DB") != 0 && strcmp(v, "BD") != 0) {
                usage_line(stderr);
                fprintf(stderr, "%s: error: argument -order: invalid choice: '%s' (choose from 'DB', 'BD')\n", PROG, v);
                return 2;
            }
            strcpy(opt->order, v);
        } else if (a[0] == '-' && a[1] != 0) {
            usage_line(stderr);
            fprintf(stderr, "%s: error: unrecognized arguments: %s\n", PROG, a);
            return 2;
        } else if (*input == NULL) {
            *input = a;
        } else {
            usage_line(stderr);
            fprintf(stderr, "%s: error: unrecognized arguments: %s\n", PROG, a);
            return 2;
        }
    }
    if (*input == NULL) {
        usage_line(stderr);
        fprintf(stderr, "%s: error: the following arguments are required: input\n", PROG);
        return 2;
    }
    return 0;
}

int main(int argc, char **argv) {
    banner();
    Options opt;
    memset(&opt, 0, sizeof(opt));
    const char *input = NULL;
    int undo = 0;
    int pr = parse_args(argc, argv, &input, &opt, &undo);
    if (pr != 0) return pr < 0 ? 0 : pr;

    ErrCtx err;
    g_err = &err;
    if (setjmp(err.jb) != 0) {
        fprintf(stderr, "Error: %s\n", err.msg);
        return 2;
    }

    if (undo) {
        if (is_dir(input)) {
            fprintf(stderr, "Error: -undo requires a single ISO file, not a folder.\n");
            return 2;
        }
        int n = run_undo(input);
        printf("Restored %d sector(s). Image is back to its previous state.\n", n);
        return 0;
    }

    if (is_dir(input)) return run_folder(input, &opt);
    if (!is_file(input)) {
        fprintf(stderr, "Error: not a file or folder: %s\n", input);
        return 2;
    }
    return run_single(input, &opt);
}
