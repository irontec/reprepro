#ifndef REPREPRO_DATABASE_P_H
#define REPREPRO_DATABASE_P_H

#ifndef REPREPRO_DATABASE_H
#include "database.h"
#endif

struct references;
struct filesdb;

struct database {
	char *directory;
	/* for the files database: */
	char *mirrordir;
	struct table *files, *contents;
	/* for the references database: */
	struct references *references;
	/* internal stuff: */
	bool locked, verbose;
	int dircreationdepth;
	bool nopackages, readonly, packagesdatabaseopen;
	char *version, *lastsupportedversion,
	     *dbversion, *lastsupporteddbversion;
	struct {
		bool createnewtables;
	} capabilities ;
};

retvalue database_listsubtables(struct database *,const char *,/*@out@*/struct strlist *);
retvalue database_dropsubtable(struct database *, const char *table, const char *subtable);

#ifdef DB_VERSION_MAJOR
retvalue database_opentable(struct database *, const char *, const char *, DBTYPE, u_int32_t preflags, int (*)(DB *,const DBT *,const DBT *), bool readonly, /*@out@*/DB **);
#define CLEARDBT(dbt) {memset(&dbt,0,sizeof(dbt));}
#define SETDBT(dbt,datastr) {const char *my = datastr;memset(&dbt,0,sizeof(dbt));dbt.data=(void *)my;dbt.size=strlen(my)+1;}
#define SETDBTl(dbt,datastr,datasize) {const char *my = datastr;memset(&dbt,0,sizeof(dbt));dbt.data=(void *)my;dbt.size=datasize;}
#endif

#endif