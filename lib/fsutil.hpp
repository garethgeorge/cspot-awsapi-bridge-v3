#ifndef FSUTIL_HPP
#define FSUTIL_HPP

#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>

int rmrfdir(const char *dir)
{
    int ret = 0;
    FTS *ftsp = NULL;
    FTSENT *curr;

    char *files[] = { (char *) dir, NULL };

    ftsp = fts_open(files, FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, NULL);
    if (!ftsp) {
        fprintf(stderr, "%s: fts_open failed: %s\n", dir, strerror(errno));
        ret = -1;
        goto finish;
    }

    while ((curr = fts_read(ftsp))) {
        switch (curr->fts_info) {
        case FTS_NS:
        case FTS_DNR:
        case FTS_ERR:
            fprintf(stderr, "%s: fts_read error: %s\n",
                    curr->fts_accpath, strerror(curr->fts_errno));
            break;

        case FTS_DC:
        case FTS_DOT:
        case FTS_NSOK:
            // Not reached unless FTS_LOGICAL, FTS_SEEDOT, or FTS_NOSTAT were
            // passed to fts_open()
            break;

        case FTS_D:
            // Do nothing. Need depth-first search, so directories are deleted
            // in FTS_DP
            break;

        case FTS_DP:
        case FTS_F:
        case FTS_SL:
        case FTS_SLNONE:
        case FTS_DEFAULT:
            if (remove(curr->fts_accpath) < 0) {
                fprintf(stderr, "%s: Failed to remove: %s\n",
                        curr->fts_path, strerror(errno));
                ret = -1;
            }
            break;
        }
    }

finish:
    if (ftsp) {
        fts_close(ftsp);
    }

    return ret;
}

int copy_file(const char *dstfilename, const char *srcfilename, int perms) {
	FILE *srcfile = fopen(srcfilename, "rb");
	FILE *dstfile = fopen(dstfilename, "wb");
	if (!srcfile || !dstfile) {
		if (srcfile)
			fclose(srcfile);
		if (dstfile) 
			fclose(dstfile);
		return -1;
	}

	int chr;
	while ((chr = fgetc(srcfile)) != EOF) {
		fputc(chr, dstfile);
	}

	fclose(srcfile);
	fclose(dstfile);

	return chmod(dstfilename, perms);
}

#endif 