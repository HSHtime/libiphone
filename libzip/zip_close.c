/*
  zip_close.c -- close zip archive and update changes
  Copyright (C) 1999-2012 Dieter Baron and Thomas Klausner

  This file is part of libzip, a library to manipulate ZIP archives.
  The authors can be contacted at <libzip@nih.at>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.
  3. The names of the authors may not be used to endorse or promote
     products derived from this software without specific prior
     written permission.
 
  THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS
  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/



#include "zipint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

static int add_data(struct zip *, struct zip_source *, struct zip_dirent *,
		    FILE *);
static int copy_data(FILE *, off_t, FILE *, struct zip_error *);
static int copy_source(struct zip *, struct zip_source *, FILE *);
static int write_cdir(struct zip *, struct zip_cdir *, FILE *);
static int _zip_cdir_set_comment(struct zip_cdir *, struct zip *);
static char *_zip_create_temp_output(struct zip *, FILE **);
static int _zip_torrentzip_cmp(const void *, const void *);



struct filelist {
    int idx;
    const char *name;
};



ZIP_EXTERN int
zip_close(struct zip *za)
{
    int survivors;
    int i, j, error;
    char *temp;
    FILE *out;
#ifndef _WIN32
    mode_t mask;
#endif
    struct zip_cdir *cd;
    struct zip_dirent de;
    struct filelist *filelist;
    int reopen_on_error;
    int new_torrentzip;
    enum zip_encoding_type com_enc, enc;
    int changed;

	int nentry;

    reopen_on_error = 0;

    if (za == NULL)
	return -1;

    changed = _zip_changed(za, &survivors);

    /* don't create zip files with no entries */
    if (survivors == 0) {
	if (za->zn && ((za->open_flags & ZIP_TRUNCATE) || (changed && za->zp))) {
	    if (remove(za->zn) != 0) {
		_zip_error_set(&za->error, ZIP_ER_REMOVE, errno);
		return -1;
	    }
	}
	zip_discard(za);
	return 0;
    }	       

    if (!changed) {
	zip_discard(za);
	return 0;
    }

    if ((filelist=(struct filelist *)malloc(sizeof(filelist[0])*survivors))
	== NULL)
	return -1;

    if ((cd=_zip_cdir_new(survivors, &za->error)) == NULL) {
	free(filelist);
	return -1;
    }

    for (i=0; i<survivors; i++)
	_zip_dirent_init(&cd->entry[i]);

    /* archive comment is special for torrentzip */
    if (zip_get_archive_flag(za, ZIP_AFL_TORRENT, 0)) {
	cd->comment = (char *)_zip_memdup(TORRENT_SIG "XXXXXXXX",
				  TORRENT_SIG_LEN + TORRENT_CRC_LEN,
				  &za->error);
	if (cd->comment == NULL) {
	    _zip_cdir_free(cd);
	    free(filelist);
	    return -1;
	}
	cd->comment_len = TORRENT_SIG_LEN + TORRENT_CRC_LEN;
    }
    else if (zip_get_archive_flag(za, ZIP_AFL_TORRENT, ZIP_FL_UNCHANGED) == 0) {
	if (_zip_cdir_set_comment(cd, za) == -1) {
	    _zip_cdir_free(cd);
	    free(filelist);
	    return -1;
	}
    }

    if ((temp=_zip_create_temp_output(za, &out)) == NULL) {
	_zip_cdir_free(cd);
	free(filelist);
	return -1;
    }


    /* create list of files with index into original archive  */
    nentry = (int)za->nentry;
    for (i=j=0; i<nentry; i++) {
	if (za->entry[i].state == ZIP_ST_DELETED)
	    continue;

	filelist[j].idx = i;
	filelist[j].name = zip_get_name(za, i, 0);
	j++;
    }
    if (zip_get_archive_flag(za, ZIP_AFL_TORRENT, 0))
	qsort(filelist, survivors, sizeof(filelist[0]),
	      _zip_torrentzip_cmp);

    new_torrentzip = (zip_get_archive_flag(za, ZIP_AFL_TORRENT, 0) == 1
		      && zip_get_archive_flag(za, ZIP_AFL_TORRENT,
					      ZIP_FL_UNCHANGED) == 0);
    error = 0;
    for (j=0; j<survivors; j++) {
	int new_data;

	i = filelist[j].idx;

	new_data = (ZIP_ENTRY_DATA_CHANGED(za->entry+i) || new_torrentzip || (za->entry[i].changes.valid & ZIP_DIRENT_COMP_METHOD));

	/* create new local directory entry */
	if (new_data) {
	    _zip_dirent_init(&de);

	    if (zip_get_archive_flag(za, ZIP_AFL_TORRENT, 0))
		_zip_dirent_torrent_normalize(&de);
		
	    if (za->entry[i].changes.valid & ZIP_DIRENT_COMP_METHOD) {
		de.settable.comp_method = za->entry[i].changes.comp_method;
		de.settable.valid |= ZIP_DIRENT_COMP_METHOD;
	    }

	    /* use it as central directory entry */
	    memcpy(cd->entry+j, &de, sizeof(cd->entry[j]));

	    /* set/update file name */
	    if ((za->entry[i].changes.valid & ZIP_DIRENT_FILENAME) == 0) {
		de.settable.filename = strdup(za->cdir->entry[i].settable.filename);
		cd->entry[j].settable.filename = za->cdir->entry[i].settable.filename;
		de.settable.valid |= ZIP_DIRENT_FILENAME;
		cd->entry[j].settable.valid |= ZIP_DIRENT_FILENAME;
	    }
	}
	else {
	    /* copy existing directory entries */
	    if (fseeko(za->zp, za->cdir->entry[i].offset, SEEK_SET) != 0) {
		_zip_error_set(&za->error, ZIP_ER_SEEK, errno);
		error = 1;
		break;
	    }
	    if (_zip_dirent_read(&de, za->zp, NULL, NULL, 1,
				 &za->error) != 0) {
		error = 1;
		break;
	    }
	    memcpy(cd->entry+j, za->cdir->entry+i, sizeof(cd->entry[j]));
	    if (de.bitflags & ZIP_GPBF_DATA_DESCRIPTOR) {
		de.crc = za->cdir->entry[i].crc;
		de.comp_size = za->cdir->entry[i].comp_size;
		de.uncomp_size = za->cdir->entry[i].uncomp_size;
		de.bitflags &= ~ZIP_GPBF_DATA_DESCRIPTOR;
		cd->entry[j].bitflags &= ~ZIP_GPBF_DATA_DESCRIPTOR;
	    }
	}

	if (za->entry[i].changes.valid & ZIP_DIRENT_FILENAME) {
	    free(de.settable.filename);
	    if ((de.settable.filename=strdup(za->entry[i].changes.filename)) == NULL) {
		error = 1;
		break;
	    }
	    cd->entry[j].settable.filename = za->entry[i].changes.filename;
	}

	if (za->entry[i].changes.valid & ZIP_DIRENT_EXTRAFIELD) {
	    free(de.settable.extrafield);
	    if (za->entry[i].changes.extrafield_len > 0) {
	        if ((de.settable.extrafield=(char *)malloc(za->entry[i].changes.extrafield_len)) == NULL) {
		    error = 1;
		    break;
	        }
	        memcpy(de.settable.extrafield, za->entry[i].changes.extrafield, za->entry[i].changes.extrafield_len);
            }
	    else
		de.settable.extrafield = NULL;
	    de.settable.extrafield_len = za->entry[i].changes.extrafield_len;
	    /* as the rest of cd entries, its malloc/free is done by za */
	    /* TODO unsure if this should also be set in the CD --
	     * not done for now
	    cd->entry[j].settable.extrafield = za->entry[i].changes.extrafield;
	    cd->entry[j].settable.extrafield_len = za->entry[i].changes.extrafield_len;
	    */
	}

	if (zip_get_archive_flag(za, ZIP_AFL_TORRENT, 0) == 0
	    && za->entry[i].changes.valid & ZIP_DIRENT_COMMENT) {
	    /* as the rest of cd entries, its malloc/free is done by za */
	    cd->entry[j].settable.comment = za->entry[i].changes.comment;
	    cd->entry[j].settable.comment_len = za->entry[i].changes.comment_len;
	}

	/* set general purpose bit flag for file name/comment encoding */
	enc = _zip_guess_encoding(de.settable.filename, strlen(de.settable.filename));
	com_enc = _zip_guess_encoding(cd->entry[i].settable.comment, cd->entry[i].settable.comment_len);
	if ((enc == ZIP_ENCODING_UTF8  && com_enc == ZIP_ENCODING_ASCII) ||
	    (enc == ZIP_ENCODING_ASCII && com_enc == ZIP_ENCODING_UTF8 ) ||
	    (enc == ZIP_ENCODING_UTF8  && com_enc == ZIP_ENCODING_UTF8 ))
	    de.bitflags |= ZIP_GPBF_ENCODING_UTF_8;
	else
	    de.bitflags &= ~ZIP_GPBF_ENCODING_UTF_8;
	cd->entry[i].bitflags = de.bitflags;

	cd->entry[j].offset = ftello(out);

	if (new_data) {
	    struct zip_source *zs;

	    zs = NULL;
	    if (!ZIP_ENTRY_DATA_CHANGED(za->entry+i)) {
		if ((zs=_zip_source_zip_new(za, za, i, ZIP_FL_UNCHANGED, 0, -1, NULL)) == NULL) {
		    error = 1;
		    break;
		}
	    }

	    if (add_data(za, zs ? zs : za->entry[i].source, &de, out) < 0) {
		error = 1;
		if (zs)
		    zip_source_free(zs);
		break;
	    }
	    if (zs)
		zip_source_free(zs);
	    
	    cd->entry[j].last_mod = de.last_mod;
	    cd->entry[j].settable.comp_method = de.settable.comp_method;
	    cd->entry[j].comp_size = de.comp_size;
	    cd->entry[j].uncomp_size = de.uncomp_size;
	    cd->entry[j].crc = de.crc;
	}
	else {
	    if (_zip_dirent_write(&de, out, 1, &za->error) < 0) {
		error = 1;
		break;
	    }
	    /* we just read the local dirent, file is at correct position */
	    if (copy_data(za->zp, cd->entry[j].comp_size, out,
			  &za->error) < 0) {
		error = 1;
		break;
	    }
	}

	_zip_dirent_finalize(&de);
    }

    free(filelist);

    if (!error) {
	if (write_cdir(za, cd, out) < 0)
	    error = 1;
    }
   
    /* pointers in cd entries are owned by za */
    cd->nentry = 0;
    _zip_cdir_free(cd);

    if (error) {
	_zip_dirent_finalize(&de);
	fclose(out);
	remove(temp);
	free(temp);
	return -1;
    }

    if (fclose(out) != 0) {
	_zip_error_set(&za->error, ZIP_ER_CLOSE, errno);
	remove(temp);
	free(temp);
	return -1;
    }
    
    if (za->zp) {
	fclose(za->zp);
	za->zp = NULL;
	reopen_on_error = 1;
    }
    if (_zip_rename(temp, za->zn) != 0) {
	_zip_error_set(&za->error, ZIP_ER_RENAME, errno);
	remove(temp);
	free(temp);
	if (reopen_on_error) {
	    /* ignore errors, since we're already in an error case */
	    za->zp = fopen(za->zn, "rb");
	}
	return -1;
    }
#ifndef _WIN32
    mask = umask(0);
    umask(mask);
    chmod(za->zn, 0666&~mask);
#endif

    zip_discard(za);
    free(temp);
    
    return 0;
}



static int
add_data(struct zip *za, struct zip_source *src, struct zip_dirent *de,
	 FILE *ft)
{
    off_t offstart, offdata, offend;
    struct zip_stat st;
    struct zip_source *s2;
    int ret;
    
    if (zip_source_stat(src, &st) < 0) {
	_zip_error_set_from_source(&za->error, src);
	return -1;
    }

    offstart = ftello(ft);

    if (_zip_dirent_write(de, ft, 1, &za->error) < 0)
	return -1;

    if ((st.valid & ZIP_STAT_COMP_METHOD) == 0) {
	st.valid |= ZIP_STAT_COMP_METHOD;
	st.comp_method = ZIP_CM_STORE;
    }

    if ((de->settable.valid & ZIP_DIRENT_COMP_METHOD) == 0 || de->settable.comp_method == ZIP_CM_DEFAULT) {
	de->settable.valid |= ZIP_DIRENT_COMP_METHOD;
	de->settable.comp_method = ZIP_CM_DEFLATE;
    }

    if (st.comp_method != de->settable.comp_method) {
	struct zip_source *s_store, *s_crc;
	zip_compression_implementation comp_impl;
	
	if (st.comp_method != ZIP_CM_STORE) {
	    if ((comp_impl=zip_get_compression_implementation(st.comp_method)) == NULL) {
		_zip_error_set(&za->error, ZIP_ER_COMPNOTSUPP, 0);
		return -1;
	    }
	    if ((s_store=comp_impl(za, src, st.comp_method, ZIP_CODEC_DECODE)) == NULL) {
		/* error set by comp_impl */
		return -1;
	    }
	}
	else
	    s_store = src;

	if ((s_crc=zip_source_crc(za, s_store, 0)) == NULL) {
	    if (s_store != src)
		zip_source_pop(s_store);
	    return -1;
	}

	/* XXX: deflate 0-byte files for torrentzip? */
	if (de->settable.comp_method != ZIP_CM_STORE && ((st.valid & ZIP_STAT_SIZE) == 0 || st.size != 0)) {
	    if ((comp_impl=zip_get_compression_implementation(de->settable.comp_method)) == NULL) {
		_zip_error_set(&za->error, ZIP_ER_COMPNOTSUPP, 0);
		zip_source_pop(s_crc);
		if (s_store != src)
		    zip_source_pop(s_store);
		return -1;
	    }
	    if ((s2=comp_impl(za, s_crc, de->settable.comp_method, ZIP_CODEC_ENCODE)) == NULL) {
		zip_source_pop(s_crc);
		if (s_store != src)
		    zip_source_pop(s_store);
		return -1;
	    }
	}
	else
	    s2 = s_crc;
    }
    else
	s2 = src;

    offdata = ftello(ft);
	
    ret = copy_source(za, s2, ft);
	
    if (zip_source_stat(s2, &st) < 0)
	ret = -1;
    
    while (s2 != src) {
	if ((s2=zip_source_pop(s2)) == NULL) {
	    /* XXX: set erorr */
	    ret = -1;
	    break;
	}
    }

    if (ret < 0)
	return -1;

    offend = ftello(ft);

    if (fseeko(ft, offstart, SEEK_SET) < 0) {
	_zip_error_set(&za->error, ZIP_ER_SEEK, errno);
	return -1;
    }

    de->last_mod = st.mtime;
    de->settable.comp_method = st.comp_method;
    de->crc = st.crc;
    de->uncomp_size = st.size;
    de->comp_size = offend - offdata;

    if (zip_get_archive_flag(za, ZIP_AFL_TORRENT, 0))
	_zip_dirent_torrent_normalize(de);

    if (_zip_dirent_write(de, ft, 1, &za->error) < 0)
	return -1;
    
    if (fseeko(ft, offend, SEEK_SET) < 0) {
	_zip_error_set(&za->error, ZIP_ER_SEEK, errno);
	return -1;
    }

    return 0;
}



static int
copy_data(FILE *fs, off_t len, FILE *ft, struct zip_error *error)
{
    char buf[BUFSIZE];
    int n, nn;

    if (len == 0)
	return 0;

    while (len > 0) {
	nn = len > (off_t)sizeof(buf) ? sizeof(buf) : len;
	if ((n=fread(buf, 1, nn, fs)) < 0) {
	    _zip_error_set(error, ZIP_ER_READ, errno);
	    return -1;
	}
	else if (n == 0) {
	    _zip_error_set(error, ZIP_ER_EOF, 0);
	    return -1;
	}

	if (fwrite(buf, 1, n, ft) != (size_t)n) {
	    _zip_error_set(error, ZIP_ER_WRITE, errno);
	    return -1;
	}
	
	len -= n;
    }

    return 0;
}



static int
copy_source(struct zip *za, struct zip_source *src, FILE *ft)
{
    char buf[BUFSIZE];
    zip_int64_t n;
    int ret;

    if (zip_source_open(src) < 0) {
	_zip_error_set_from_source(&za->error, src);
	return -1;
    }

    ret = 0;
    while ((n=zip_source_read(src, buf, sizeof(buf))) > 0) {
	if (fwrite(buf, 1, n, ft) != (size_t)n) {
	    _zip_error_set(&za->error, ZIP_ER_WRITE, errno);
	    ret = -1;
	    break;
	}
    }
    
    if (n < 0) {
	if (ret == 0)
	    _zip_error_set_from_source(&za->error, src);
	ret = -1;
    }	

    zip_source_close(src);
    
    return ret;
}



static int
write_cdir(struct zip *za, struct zip_cdir *cd, FILE *out)
{
    off_t offset;
    uLong crc;
    char buf[TORRENT_CRC_LEN+1];
    
    if (_zip_cdir_write(cd, out, &za->error) < 0)
	return -1;
    
    if (zip_get_archive_flag(za, ZIP_AFL_TORRENT, 0) == 0)
	return 0;


    /* fix up torrentzip comment */

    offset = ftello(out);

    if (_zip_filerange_crc(out, cd->offset, cd->size, &crc, &za->error) < 0)
	return -1;

    snprintf(buf, sizeof(buf), "%08lX", (long)crc);

    if (fseeko(out, offset-TORRENT_CRC_LEN, SEEK_SET) < 0) {
	_zip_error_set(&za->error, ZIP_ER_SEEK, errno);
	return -1;
    }

    if (fwrite(buf, TORRENT_CRC_LEN, 1, out) != 1) {
	_zip_error_set(&za->error, ZIP_ER_WRITE, errno);
	return -1;
    }

    return 0;
}



static int
_zip_cdir_set_comment(struct zip_cdir *dest, struct zip *src)
{
    if (src->ch_comment_len != -1) {
	dest->comment_len = src->ch_comment_len;
	if (src->ch_comment_len == 0)
	    dest->comment = NULL;
	else {
	    dest->comment = (char *)_zip_memdup(src->ch_comment,
				        src->ch_comment_len, &src->error);
	    if (dest->comment == NULL)
	        return -1;
	}
    }
    else {
	if (src->cdir && src->cdir->comment_len > 0) {
	    dest->comment = (char *)_zip_memdup(src->cdir->comment,
					src->cdir->comment_len, &src->error);
	    if (dest->comment == NULL)
		return -1;
	    dest->comment_len = src->cdir->comment_len;
	}
    }

    return 0;
}



int
_zip_changed(struct zip *za, int *survivorsp)
{
    int changed, i, survivors;

	int nentry;

    changed = survivors = 0;

    if (za->ch_comment_len != -1
	|| za->ch_flags != za->flags)
	changed = 1;

    nentry = (int)za->nentry;
    for (i=0; i<nentry; i++) {
	if (za->entry[i].state != ZIP_ST_UNCHANGED || za->entry[i].changes.valid != 0)
	    changed = 1;
	if (za->entry[i].state != ZIP_ST_DELETED)
	    survivors++;
    }

    if (survivorsp)
	*survivorsp = survivors;

    return changed;
}



static char *
_zip_create_temp_output(struct zip *za, FILE **outp)
{
    char *temp;
    int tfd;
    FILE *tfp;
    
    if ((temp=(char *)malloc(strlen(za->zn)+8)) == NULL) {
	_zip_error_set(&za->error, ZIP_ER_MEMORY, 0);
	return NULL;
    }

    sprintf(temp, "%s.XXXXXX", za->zn);

    if ((tfd=mkstemp(temp)) == -1) {
	_zip_error_set(&za->error, ZIP_ER_TMPOPEN, errno);
	free(temp);
	return NULL;
    }
    
    if ((tfp=fdopen(tfd, "r+b")) == NULL) {
	_zip_error_set(&za->error, ZIP_ER_TMPOPEN, errno);
	close(tfd);
	remove(temp);
	free(temp);
	return NULL;
    }

#ifdef _WIN32
    /*
      According to Pierre Joye, Windows in some environments per
      default creates text files, so force binary mode.
    */
    _setmode(_fileno(tfp), _O_BINARY );
#endif

    *outp = tfp;
    return temp;
}



static int
_zip_torrentzip_cmp(const void *a, const void *b)
{
    return strcasecmp(((const struct filelist *)a)->name,
		      ((const struct filelist *)b)->name);
}
