/*  This file is part of "mirrorer" (TODO: find better title)
 *  Copyright (C) 2003 Bernhard R. Link
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <config.h>

#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <malloc.h>
#include <ctype.h>
#include <db.h>
#include "error.h"
#include "strlist.h"
#include "md5sum.h"
#include "names.h"
#include "dirs.h"
#include "chunks.h"
#include "reference.h"
#include "packages.h"
#include "signature.h"
#include "sources.h"
#include "files.h"
#include "guesscomponent.h"
#include "checkindsc.h"
#include "checkindeb.h"
#include "checkin.h"

extern int verbose;

/* Things to do when including a .changes-file:
 *  - Read in the chunk of the possible signed file.
 *    (In later versions possibly checking the signature)
 *  - Parse it, extracting:
 *  	+ Distribution
 * 	+ Source
 * 	+ Architecture
 * 	+ Binary
 * 	+ Version
 * 	+ ...
 * 	+ Files
 *  - Calculate what files are expectable...
 *  - Compare supplied filed with files expected.
 *  - (perhaps: write what was done and changes to some logfile)
 *  - add supplied files to the pool and register them in files.db
 *  - add the .dsc-files via checkindsc.c
 *  - add the .deb-filed via checkindeb.c
 *
 */

typedef	enum { fe_UNKNOWN=0,fe_DEB,fe_UDEB,fe_DSC,fe_DIFF,fe_ORIG,fe_TAR} filetype;

struct fileentry {
	struct fileentry *next;
	char *basename;
	filetype type;
	char *md5andsize;
	char *section;
	char *priority;
	char *architecture;
	char *name;
	/* only set after changes_includefiles */
	char *filekey;
};

struct changes {
	/* Things read by changes_read: */
	char *source, *version;
	struct strlist distributions,
		       architectures,
		       binaries;
	struct fileentry *files;
	char *control;
	/* Things to be set by changes_fixfields: */
	char *component,*directory;
};

static void freeentries(struct fileentry *entry) {
	struct fileentry *h;

	while( entry ) {
		h = entry->next;
		free(entry->filekey);
		free(entry->basename);
		free(entry->md5andsize);
		free(entry->section);
		free(entry->priority);
		free(entry->architecture);
		free(entry->name);
		free(entry);
		entry = h;
	}
}

static void changes_free(struct changes *changes) {
	if( changes != NULL ) {
		free(changes->source);
		free(changes->version);
		strlist_done(&changes->architectures);
		strlist_done(&changes->binaries);
		freeentries(changes->files);
		strlist_done(&changes->distributions);
		free(changes->control);
		free(changes->component);
		free(changes->directory);
	}
	free(changes);
}


static retvalue newentry(struct fileentry **entry,const char *fileline) {
	struct fileentry *e;
	const char *p,*md5start,*md5end;
	const char *sizestart,*sizeend;
	const char *sectionstart,*sectionend;
	const char *priostart,*prioend;
	const char *filestart,*nameend,*fileend;
	const char *archstart,*archend;
	const char *versionstart,*typestart;
	filetype type;

	p = fileline;
	while( *p && isspace(*p) )
		p++;
	md5start = p;
	while( *p && !isspace(*p) )
		p++;
	md5end = p;
	while( *p && isspace(*p) )
		p++;
	sizestart = p;
	while( *p && !isspace(*p) )
		p++;
	sizeend = p;
	while( *p && isspace(*p) )
		p++;
	sectionstart = p;
	while( *p && !isspace(*p) )
		p++;
	sectionend = p;
	while( *p && isspace(*p) )
		p++;
	priostart = p;
	while( *p && !isspace(*p) )
		p++;
	prioend = p;
	while( *p && isspace(*p) )
		p++;
	filestart = p;
	while( *p && !isspace(*p) )
		p++;
	fileend = p;
	while( *p && isspace(*p) )
		p++;
	if( *p != '\0' ) {
		fprintf(stderr,"Unexpected sixth argument in '%s'!\n",fileline);
		return RET_ERROR;
	}
	if( *md5start == '\0' || *sizestart == '\0' || *sectionstart == '\0'
			|| *priostart == '\0' || *filestart == '\0' ) {
		fprintf(stderr,"Not five arguments in '%s'!\n",fileline);
		return RET_ERROR;
	}

	p = filestart;
	names_overpkgname(&p);
	if( *p != '_' ) {
		if( *p == '\0' )
			fprintf(stderr,"No underscore in filename in '%s'!",fileline);
		else
			fprintf(stderr,"Unexpected character '%c' in filename in '%s'\n!",*p,fileline);
		return RET_ERROR;
	}
	nameend = p;
	p++;
	versionstart = p;
	// We cannot say where the version ends and the filename starts,
	// but as the suffixes would be valid part of the version, too,
	// this check gets the broken things. 
	names_overversion(&p);
	if( *p != '\0' && *p != '_' ) {
		fprintf(stderr,"Unexpected character '%c' in filename within '%s'!\n",*p,fileline);
		return RET_ERROR;
	}
	if( *p == '_' ) {
		/* Things having a underscole will have an architecture
		 * and be either .deb or .udeb */
		p++;
		archstart = p;
		while( *p && *p != '.' )
			p++;
		if( *p != '.' ) {
			fprintf(stderr,"Expect something of the vorm name_version_arch.[u]deb but got '%s'!\n",filestart);
			return RET_ERROR;
		}
		archend = p;
		p++;
		typestart = p;
		while( *p && !isspace(*p) )
			p++;
		if( p-typestart == 3 && strncmp(typestart,"deb",3) == 0 )
			type = fe_DEB;
		else if( p-typestart == 4 && strncmp(typestart,"udeb",4) == 0 )
			type = fe_UDEB;
		else {
			fprintf(stderr,"'%s' looks neighter like .deb nor like .udeb!\n",filestart);
			return RET_ERROR;
		}
		if( strncmp(archstart,"source",6) == 0 ) {
			fprintf(stderr,"How can a .[u]deb be architecture 'source'?('%s')\n",filestart);
			return RET_ERROR;
		}
	} else {
		/* this looks like some source-package, we will have
		 * to look for the suffix ourself... */
		while( *p && !isspace(*p) ) {
			p++;
		}
		if( p-versionstart > 12 && strncmp(p-12,".orig.tar.gz",12) == 0 )
			type = fe_ORIG;
		else if( p-versionstart > 7 && strncmp(p-7,".tar.gz",7) == 0 )
			type = fe_TAR;
		else if( p-versionstart > 8 && strncmp(p-8,".diff.gz",8) == 0 )
			type = fe_DIFF;
		else if( p-versionstart > 4 && strncmp(p-4,".dsc",4) == 0 )
			type = fe_DSC;
		else {
			type = fe_UNKNOWN;
			fprintf(stderr,"Unknown filetype: '%s', assuming to be source format...\n",fileline);
		}
		archstart = "source";
		archend = archstart + 6;
	}
	/* now copy all those parts into the structure */
	e = calloc(1,sizeof(struct fileentry));
	if( e == NULL )
		return RET_ERROR_OOM;
	e->md5andsize = names_concatmd5sumandsize(md5start,md5end,sizestart,sizeend);
	e->section = strndup(sectionstart,sectionend-sectionstart);
	e->priority = strndup(priostart,prioend-priostart);
	e->basename = strndup(filestart,fileend-filestart);
	e->architecture = strndup(archstart,archend-archstart);
	e->name = strndup(filestart,nameend-filestart);
	e->type = type;

	if( !e->basename || !e->md5andsize || !e->section || !e->priority || !e->architecture || !e->name ) {
		freeentries(e);
		return RET_ERROR_OOM;
	}
	e->next = *entry;
	*entry = e;
	return RET_OK;
}

/* Parse the Files-header to see what kind of files we carry around */
static retvalue changes_parsefilelines(const char *filename,struct changes *changes,const struct strlist *filelines,int force) {
	retvalue r;
	int i;

	assert( changes->files == NULL);
	r = RET_NOTHING;

	for( i = 0 ; i < filelines->count ; i++ ) {
		const char *fileline = filelines->values[i];

		r = newentry(&changes->files,fileline);
		if( r == RET_ERROR )
			return r;
	}
	if( r == RET_NOTHING ) {
		fprintf(stderr,"%s: Not enough files in .changes!\n",filename);
		return RET_ERROR;
	}
	return r;
}

static retvalue check(const char *filename,struct changes *changes,const char *field,int force) {
	retvalue r;

	r = chunk_checkfield(changes->control,field);
	if( r == RET_NOTHING ) {
		fprintf(stderr,"In '%s': Missing '%s' field!\n",filename,field);
		if( !force )
			return RET_ERROR;
	}
	return r;
}

static retvalue changes_read(const char *filename,struct changes **changes,int force) {
	retvalue r;
	struct changes *c;
	struct strlist filelines;

#define E(err,param...) { \
		if( r == RET_NOTHING ) { \
			fprintf(stderr,"In '%s': " err "\n",filename , ## param ); \
			r = RET_ERROR; \
	  	} \
		if( RET_WAS_ERROR(r) ) { \
			changes_free(c); \
			return r; \
		} \
	}
#define C(err,param...) { \
		if( RET_WAS_ERROR(r) ) { \
			if( !force ) { \
				fprintf(stderr,"In '%s': " err "\n",filename , ## param ); \
				changes_free(c); \
				return r; \
			} else { \
				fprintf(stderr,"Ignoring " err " in '%s' due to --force:\n " err "\n" , ## param , filename); \
			} \
		} \
	}
#define R { \
		if( RET_WAS_ERROR(r) ) { \
			changes_free(c); \
			return r; \
		} \
	}
			
		
	c = calloc(1,sizeof(struct changes));
	if( c == NULL )
		return RET_ERROR_OOM;
	r = signature_readsignedchunk(filename,&c->control);
	R;
	r = check(filename,c,"Format",force);
	R;
	r = check(filename,c,"Date",force);
	R;
	r = chunk_getname(c->control,"Source",&c->source,0);
	E("Missing 'Source' field");
	r = names_checkpkgname(c->source);
	C("Malforce Source-field");
	r = chunk_getwordlist(c->control,"Binary",&c->binaries);
	E("Missing 'Binary' field");
	r = chunk_getwordlist(c->control,"Architecture",&c->architectures);
	E("Missing 'Architecture' field");
	r = chunk_getvalue(c->control,"Version",&c->version);
	E("Missing 'Version' field");
	r = names_checkversion(c->version);
	C("Malforce Version number");
	r = chunk_getwordlist(c->control,"Distribution",&c->distributions);
	E("Missing 'Distribution' field");
	r = check(filename,c,"Urgency",force);
	R;
	r = check(filename,c,"Maintainer",force);
	R;
	r = check(filename,c,"Description",force);
	R;
	r = check(filename,c,"Changes",force);
	R;
	r = chunk_getextralinelist(c->control,"Files",&filelines);
	E("Missing 'Files' field");
	r = changes_parsefilelines(filename,c,&filelines,force);
	strlist_done(&filelines);
	R;

	*changes = c;
	return RET_OK;
#undef E
#undef C
#undef R
}

static retvalue changes_fixfields(const struct distribution *distribution,const char *filename,struct changes *changes,const char *forcecomponent,const char *forcepriority,const char *forcesection,int force) {
	struct fileentry *e;

	e = changes->files;

	if( e == NULL ) {
		fprintf(stderr,"No files given in '%s'!\n",filename);
		return RET_ERROR;
	}
	
	while( e ) {
		if( forcesection ) {
			free(e->section);
			e->section = strdup(forcesection);
			if( e->section == NULL )
				return RET_ERROR_OOM;
		} else {
		// TODO: otherwise check overwrite file...
		}
		if( strcmp(e->section,"unknown") == 0 ) {
			fprintf(stderr,"Section '%s' of '%s' is not valid!\n",e->section,filename);
			return RET_ERROR;
		}
		if( strcmp(e->section,"byhand" ) == 0 ) {
			fprintf(stderr,"Cannot cope with'byhand' file '%s'!\n",e->basename);
			return RET_ERROR;
		}
		if( strcmp(e->section,"-") == 0 ) {
			fprintf(stderr,"No section specified for of '%s'!\n",filename);
			return RET_ERROR;
		}
		if( forcepriority ) {
			free(e->priority);
			e->priority = strdup(forcepriority);
			if( e->priority == NULL )
				return RET_ERROR_OOM;
		};
		if( strcmp(e->priority,"-") == 0 ) {
			fprintf(stderr,"No priority specified for of '%s'!\n",filename);
			return RET_ERROR;
		}

		if( forcecomponent == NULL ) {
			char *component;
			retvalue r;

			r = guess_component(distribution->codename,&distribution->components,changes->source,e->section,forcecomponent,&component);
			if( RET_WAS_ERROR(r) )
				return r;

			if( changes->component ) {
				if( strcmp(changes->component,component) != 0)  {
					fprintf(stderr,"%s contains files guessed to be in different components ('%s' vs '%s)!\n",filename,component,changes->component);
					free(component);
					return RET_ERROR;
				}
				free(component);

			} else {
				changes->component = component;
			}

		}

		e = e->next;
	}
	if( forcecomponent ) {
		changes->component = strdup(forcecomponent);
		if( changes->component == NULL )
			return RET_ERROR_OOM;
	} else
		assert( changes->component != NULL);

	changes->directory = calc_sourcedir(changes->component,changes->source);
	if( changes->directory == NULL )
		return RET_ERROR_OOM;

	return RET_OK;
}

static retvalue changes_check(const char *filename,struct changes *changes,int force) {
	int i;
	struct fileentry *e;
	retvalue r = RET_OK;
	int havedsc=0, haveorig=0, havetar=0, havediff=0;
	
	/* First check for each given architecture, if it has files: */
	for( i = 0 ; i < changes->architectures.count ; i++ ) {
		const char *architecture = changes->architectures.values[i];
		
		e = changes->files;
		while( e && strcmp(e->architecture,architecture) != 0 )
			e = e->next;
		if( e == NULL ) {
			fprintf(stderr,"Architecture-header in '%s' lists architecture '%s', but no files for this!\n",filename,architecture);
			r = RET_ERROR;
		}
	}
	/* Then check for each file, if its architecture is sensible
	 * and listed. */
	e = changes->files;
	while( e ) {
		if( !strlist_in(&changes->architectures,e->architecture) ) {
			fprintf(stderr,"'%s' looks like architecture '%s', but this is not listed in the Architecture-Header!\n",filename,e->architecture);
			r = RET_ERROR;
		}
		if( e->type == fe_DSC ) {
			char *calculatedname;
			if( havedsc ) {
				fprintf(stderr,"I don't know what to do with multiple .dsc files in '%s'!\n",filename);
				return RET_ERROR;
			}
			havedsc = 1;
			calculatedname = calc_source_basename(changes->source,changes->version);
			if( calculatedname == NULL )
				return RET_ERROR_OOM;
			if( strcmp(calculatedname,e->basename) != 0 ) {
				free(calculatedname);
				fprintf(stderr,"dsc-filename is '%s' instead of the expected '%s'!\n",e->basename,calculatedname);
				return RET_ERROR;
			}
			free(calculatedname);
		} else if( e->type == fe_DIFF ) {
			if( havediff ) {
				fprintf(stderr,"I don't know what to do with multiple .diff files in '%s'!\n",filename);
				return RET_ERROR;
			}
			havediff = 1;
		} else if( e->type == fe_ORIG ) {
			if( haveorig ) {
				fprintf(stderr,"I don't know what to do with multiple .orig.tar.gz files in '%s'!\n",filename);
				return RET_ERROR;
			}
			haveorig = 1;
		} else if( e->type == fe_TAR ) {
			if( havetar ) {
				fprintf(stderr,"I don't know what to do with multiple .tar.gz files in '%s'!\n",filename);
				return RET_ERROR;
			}
			havetar = 1;
		}

		e = e->next;
	}

	if( havetar && haveorig ) {
		fprintf(stderr,"I don't know what to do having a .tar.gz and a .orig.tar.gz in '%s'!\n",filename);
		return RET_ERROR;
	}
	if( havetar && havediff ) {
		fprintf(stderr,"I don't know what to do having a .tar.gz not beeing a .orig.tar.gz and a .diff.gz in '%s'!\n",filename);
		return RET_ERROR;
	}
	if( strlist_in(&changes->architectures,"source") && !havedsc ) {
		fprintf(stderr,"I don't know what to do with a source-upload not containing a .dsc in '%s'!\n",filename);
		return RET_ERROR;
	}
	if( havedsc && !havediff && !havetar ) {
		fprintf(stderr,"I don't know what to do having a .dsc without a .diff.gz or .tar.gz in '%s'!\n",filename);
		return RET_ERROR;
	}

	return r;
}

static retvalue changes_includefiles(filesdb filesdb,const char *component,const char *filename,struct changes *changes,int force) {
	struct fileentry *e;
	retvalue r;
	char *sourcedir; 

	r = dirs_getdirectory(filename,&sourcedir);
	if( RET_WAS_ERROR(r) )
		return r;

	r = RET_NOTHING;

	e = changes->files;
	while( e ) {
		e->filekey = calc_dirconcat(changes->directory,e->basename);

		if( e->filekey == NULL ) {
			free(sourcedir);
			return RET_ERROR_OOM;
		}
		r = files_checkinfile(filesdb,sourcedir,e->basename,e->filekey,e->md5andsize);
		if( RET_WAS_ERROR(r) )
			break;
		e = e->next;
	}

	free(sourcedir);
	return r;
}

static retvalue changes_includepkgs(const char *dbdir,DB *references,filesdb filesdb,struct distribution *distribution,struct changes *changes,int force) {
	struct fileentry *e;
	retvalue r;

	r = RET_NOTHING;

	e = changes->files;
	while( e ) {
		char *fullfilename;
		if( e->type != fe_DEB && e->type != fe_DSC ) {
			e = e->next;
			continue;
		}
		fullfilename = calc_dirconcat(filesdb->mirrordir,e->filekey);
		if( fullfilename == NULL )
			return RET_ERROR_OOM;
		// TODO: give directory and filekey, too, so that they
		// do not have to be calculated again. (and the md5sums fit)
		if( e->type == fe_DEB ) {
			r = deb_add(dbdir,references,filesdb,
				changes->component,e->section,e->priority,
				distribution,fullfilename,
				e->filekey,e->md5andsize,
				force);
		} else if( e->type == fe_DSC ) {
			r = dsc_add(dbdir,references,filesdb,
				changes->component,e->section,e->priority,
				distribution,fullfilename,
				e->filekey,e->basename,
				changes->directory,e->md5andsize,
				force);
		}
		
		free(fullfilename);
		if( RET_WAS_ERROR(r) )
			break;
		e = e->next;
	}

	return r;
}

/* insert the given .changes into the mirror in the <distribution>
 * if forcecomponent, forcesection or forcepriority is NULL
 * get it from the files or try to guess it. */
retvalue changes_add(const char *dbdir,DB *references,filesdb filesdb,const char *forcecomponent,const char *forcesection,const char *forcepriority,struct distribution *distribution,const char *changesfilename,int force) {
	retvalue r;
	struct changes *changes;

	r = changes_read(changesfilename,&changes,force);
	if( RET_WAS_ERROR(r) )
		return r;
	if( changes->distributions.count != 1 ) {
		fprintf(stderr,"There is not exactly one distribution given!\n");
		changes_free(changes);
		return RET_ERROR;
	}
	if( !strlist_in(&changes->distributions,distribution->suite) &&
	    !strlist_in(&changes->distributions,distribution->codename) ) {
		fprintf(stderr,"Warning: .changes put in a distribution not listed!\n");
	}
	/* look for component, section and priority to be correct or guess them*/
	r = changes_fixfields(distribution,changesfilename,changes,forcecomponent,forcesection,forcepriority,force);
	if( RET_WAS_ERROR(r) ) {
		changes_free(changes);
		return r;
	}
	/* do some tests if values are sensible */
	r = changes_check(changesfilename,changes,force);
	if( RET_WAS_ERROR(r) ) {
		changes_free(changes);
		return r;
	}
	
	/* add files in the pool */
	r = changes_includefiles(filesdb,changes->component,changesfilename,changes,force);
	if( RET_WAS_ERROR(r) ) {
		changes_free(changes);
		return r;
	}

	/* add the source and binary packages in the given distribution */
	r = changes_includepkgs(dbdir,references,filesdb,
		distribution,changes,force);
	if( RET_WAS_ERROR(r) ) {
		changes_free(changes);
		return r;
	}

	return RET_OK;
}