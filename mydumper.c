#include <mysql.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <zlib.h>
#include <glib/gstdio.h>

struct configuration {
	char use_any_index;
	GAsyncQueue *queue;
	GAsyncQueue *ready;
	GMutex *mutex;
	int done;
};

/* Database options */
char *hostname=NULL;
char *username=NULL;
char *password=NULL;
char *db=NULL;
guint port=3306;

#define DIRECTORY "export"

/* Program options */
guint num_threads = 4;
gchar *directory = NULL;
guint statement_size = 1000000;
guint rows_per_file = 0;

int need_dummy_read=0;
int compress_output=0;

static GOptionEntry entries[] =
{
	{ "host", 'h', 0, G_OPTION_ARG_STRING, &hostname, "The host to connect to", NULL },
	{ "user", 'u', 0, G_OPTION_ARG_STRING, &username, "Username with privileges to run the dump", NULL },
	{ "password", 'p', 0, G_OPTION_ARG_STRING, &password, "User password", NULL },
	{ "port", 'P', 0, G_OPTION_ARG_INT, &port, "TCP/IP port to connect to", NULL },
	{ "database", 'B', 0, G_OPTION_ARG_STRING, &db, "Database to dump", NULL },
	{ "threads", 't', 0, G_OPTION_ARG_INT, &num_threads, "Number of parallel threads", NULL },
	{ "outputdir", 'o', 0, G_OPTION_ARG_FILENAME, &directory, "Directory to output files to, default ./" DIRECTORY"-*/",  NULL },
	{ "statement-size", 's', 0, G_OPTION_ARG_INT, &statement_size, "Attempted size of INSERT statement in bytes", NULL},
	{ "rows", 'r', 0, G_OPTION_ARG_INT, &rows_per_file, "Try to split tables into chunks of this many rows", NULL},
	{ "compress", 'c', 0, G_OPTION_ARG_NONE, &compress_output, "Compress output files", NULL},
	{ NULL }
};

enum job_type { JOB_SHUTDOWN, JOB_DUMP };

struct job {
	enum job_type type;
	char *database;
	char *table;
	char *filename;
	char *where;
	struct configuration *conf;
};

struct tm tval;

void dump_table(MYSQL *conn, char *database, char *table, struct configuration *conf);
void dump_table_data(MYSQL *, FILE *, char *, char *, char *, struct configuration *conf);
void dump_database(MYSQL *, char *, struct configuration *conf);
GList * get_chunks_for_table(MYSQL *, char *, char *, struct configuration *conf);
guint64 estimate_count(MYSQL *conn, char *database, char *table, char *field, char *from, char *to);
void dump_table_data_file(MYSQL *conn, char *database, char *table, char *where, char *filename, struct configuration *conf);
void create_backup_dir(char *directory);
int write_data(void *file,GString *);

/* Write some stuff we know about snapshot, before it changes */
void write_snapshot_info(MYSQL *conn, FILE *file) {
	MYSQL_RES *master=NULL, *slave=NULL;
	MYSQL_FIELD *fields;
	MYSQL_ROW row;

	char *masterlog=NULL;
	char *masterpos=NULL;

	char *slavehost=NULL;
	char *slavelog=NULL;
	char *slavepos=NULL;

	mysql_query(conn,"SHOW MASTER STATUS");
	master=mysql_store_result(conn);
	if (master && (row=mysql_fetch_row(master))) {
		masterlog=row[0];
		masterpos=row[1];
	}

	mysql_query(conn, "SHOW SLAVE STATUS");
	slave=mysql_store_result(conn);
	int i;
	if (slave && (row=mysql_fetch_row(slave))) {
		fields=mysql_fetch_fields(slave);
		for (i=0; i<mysql_num_fields(slave);i++) {
			if (!strcasecmp("exec_master_log_pos",fields[i].name)) {
				slavepos=row[i];
			} else if (!strcasecmp("relay_master_log_file", fields[i].name)) {
				slavelog=row[i];
			} else if (!strcasecmp("master_host",fields[i].name)) {
				slavehost=row[i];
			}
		}
	}

	if (masterlog)
		fprintf(file, "SHOW MASTER STATUS:\n\tLog: %s\n\tPos: %s\n\n", masterlog, masterpos);

	if (slavehost)
		fprintf(file, "SHOW SLAVE STATUS:\n\tHost: %s\n\tLog: %s\n\tPos: %s\n\n",
			slavehost, slavelog, slavepos);

	fflush(file);
	if (master)
		mysql_free_result(master);
	if (slave)
		mysql_free_result(slave);
}

void *process_queue(struct configuration * conf) {
	mysql_thread_init();
	MYSQL *thrconn = mysql_init(NULL);
	mysql_options(thrconn,MYSQL_READ_DEFAULT_GROUP,"mydumper");

	if(!mysql_real_connect(thrconn, hostname, username, password, db, port, NULL, 0)) {
		g_critical("Failed to connect to database: %s", mysql_error(thrconn));
		exit(EXIT_FAILURE);
	}
	if (mysql_query(thrconn, "START TRANSACTION /*!40108 WITH CONSISTENT SNAPSHOT */")) {
		g_critical("Failed to start consistent snapshot: %s",mysql_error(thrconn));
	}
	/* Unfortunately version before 4.1.8 did not support consistent snapshot transaction starts, so we cheat */
	if (need_dummy_read) {
		mysql_query(thrconn,"SELECT * FROM mysql.mydumperdummy");
		MYSQL_RES *res=mysql_store_result(thrconn);
		if (res)
			mysql_free_result(res);
	}
	mysql_query(thrconn, "/*!40101 SET NAMES binary*/");

	g_async_queue_push(conf->ready,GINT_TO_POINTER(1));

	struct job* job;
	for(;;) {
		GTimeVal tv;
		g_get_current_time(&tv);
		g_time_val_add(&tv,1000*1000*1);
		job=g_async_queue_pop(conf->queue);
		switch (job->type) {
			case JOB_DUMP:
				dump_table_data_file(thrconn, job->database, job->table, job->where, job->filename, job->conf);
				break;
			case JOB_SHUTDOWN:
				if (thrconn)
					mysql_close(thrconn);
				g_free(job);
				mysql_thread_end();
				return NULL;
				break;
		}
		if(job->database) g_free(job->database);
		if(job->table) g_free(job->table);
		if(job->where) g_free(job->where);
		if(job->filename) g_free(job->filename);
		g_free(job);
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	struct configuration conf = { 1, NULL, NULL, NULL, 0 };

	GError *error = NULL;
	GOptionContext *context;

	g_thread_init(NULL);
	
	context = g_option_context_new("multi-threaded MySQL dumping");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error))
	{
		g_print ("option parsing failed: %s, try --help\n", error->message);
		exit (EXIT_FAILURE);
	}
	g_option_context_free(context);

	time_t t;
	time(&t);localtime_r(&t,&tval);

	if (!directory)
		directory = g_strdup_printf("%s-%04d%02d%02d-%02d%02d%02d",DIRECTORY,
			tval.tm_year+1900, tval.tm_mon+1, tval.tm_mday,
			tval.tm_hour, tval.tm_min, tval.tm_sec);

	create_backup_dir(directory);

	char *p;
	FILE* mdfile=g_fopen(p=g_strdup_printf("%s/.metadata",directory),"w");
	g_free(p);
	if(!mdfile) {
		g_critical("Couldn't write metadata file (%d)",errno);
		exit(1);
	}

	MYSQL *conn;
	conn = mysql_init(NULL);
	mysql_options(conn,MYSQL_READ_DEFAULT_GROUP,"mydumper");

	if (!mysql_real_connect(conn, hostname, username, password, db, port, NULL, 0)) {
		g_critical("Error connecting to database: %s", mysql_error(conn));
		exit(EXIT_FAILURE);
	}
	if (mysql_query(conn, "FLUSH TABLES WITH READ LOCK"))
		g_warning("Couldn't acquire global lock, snapshots will not be consistent: %s",mysql_error(conn));

	if (mysql_get_server_version(conn)) {
		mysql_query(conn, "CREATE TABLE IF NOT EXISTS mysql.mydumperdummy (a INT) ENGINE=INNODB");
		need_dummy_read=1;
	}
	mysql_query(conn, "START TRANSACTION /*!40108 WITH CONSISTENT SNAPSHOT */");
	if (need_dummy_read) {
		mysql_query(conn,"SELECT * FROM mysql.mydumperdummy");
		MYSQL_RES *res=mysql_store_result(conn);
		if (res)
			mysql_free_result(res);
	}

	time(&t);localtime_r(&t,&tval);
	fprintf(mdfile,"Started dump at: %04d-%02d-%02d %02d:%02d:%02d\n",
		tval.tm_year+1900, tval.tm_mon+1, tval.tm_mday, 
		tval.tm_hour, tval.tm_min, tval.tm_sec);

	mysql_query(conn, "/*!40101 SET NAMES binary*/");

	write_snapshot_info(conn, mdfile);

	conf.queue = g_async_queue_new();
	conf.ready = g_async_queue_new();

	int n;
	GThread **threads = g_new(GThread*,num_threads);
	for (n=0; n<num_threads; n++) {
		threads[n] = g_thread_create((GThreadFunc)process_queue,&conf,TRUE,NULL);
		g_async_queue_pop(conf.ready);
	}
	g_async_queue_unref(conf.ready);
	mysql_query(conn, "UNLOCK TABLES");

	if (db) {
		dump_database(conn, db, &conf);
	} else {
		MYSQL_RES *databases;
		MYSQL_ROW row;
		if(mysql_query(conn,"SHOW DATABASES")) {
			g_critical("Unable to list databases: %s",mysql_error(conn));
		}
		databases = mysql_store_result(conn);
		while ((row=mysql_fetch_row(databases))) {
			if (!strcmp(row[0],"information_schema"))
				continue;
			dump_database(conn, row[0], &conf);
		}
		mysql_free_result(databases);
	}

	for (n=0; n<num_threads; n++) {
		struct job *j = g_new0(struct job,1);
		j->type = JOB_SHUTDOWN;
		g_async_queue_push(conf.queue,j);
	}
	
	for (n=0; n<num_threads; n++) {
		g_thread_join(threads[n]);
	}
	g_async_queue_unref(conf.queue);

	time(&t);localtime_r(&t,&tval);
	fprintf(mdfile,"Finished dump at: %04d-%02d-%02d %02d:%02d:%02d\n",
		tval.tm_year+1900, tval.tm_mon+1, tval.tm_mday,
		tval.tm_hour, tval.tm_min, tval.tm_sec);
	fclose(mdfile);

	mysql_close(conn);
	mysql_thread_end();
	mysql_library_end();
	g_free(directory);
	g_free(threads);
	return (0);
}

/*
 * Heuristic chunks building - based on estimates, produces list of ranges for datadumping
 * WORK IN PROGRESS
 */
GList * get_chunks_for_table(MYSQL *conn, char *database, char *table, struct configuration *conf)
{
	GList *chunks = NULL;
	MYSQL_RES *indexes=NULL, *minmax=NULL, *total=NULL;
	MYSQL_ROW row;
	char *index = NULL, *field = NULL;
	
	/* first have to pick index, in future should be able to preset in configuration too */
	gchar *query = g_strdup_printf("SHOW INDEX FROM `%s`.`%s`",database,table);
	mysql_query(conn,query);
	g_free(query);
	indexes=mysql_store_result(conn);
	
	while ((row=mysql_fetch_row(indexes))) {
		if (!strcmp(row[2],"PRIMARY") && (!strcmp(row[3],"1"))) {
			/* Pick first column in PK, cardinality doesn't matter */
			field=row[4];
			index=row[2];
			break;
		}
	}

	/* If no PK found, try using first UNIQUE index */
	if (!field) {
		mysql_data_seek(indexes,0);
		while ((row=mysql_fetch_row(indexes))) {
			if(!strcmp(row[1],"0") && (!strcmp(row[3],"1"))) {
				/* Again, first column of any unique index */
				field=row[4];
				index=row[2];
				break;
			}
		}
	}
	
	/* Still unlucky? Pick any high-cardinality index */
	if (!field && conf->use_any_index) {
		guint64 max_cardinality=0;
		guint64 cardinality=0;
		
		mysql_data_seek(indexes,0);
		while ((row=mysql_fetch_row(indexes))) {
			if(!strcmp(row[3],"1")) {
				if (row[6])
					cardinality = strtoll(row[6],NULL,10);
				if (cardinality>max_cardinality) {
					field=row[4];
					max_cardinality=cardinality;
				}
			}
		}
	}
	/* Oh well, no chunks today - no suitable index */
	if (!field) goto cleanup;

	/* Get minimum/maximum */
	mysql_query(conn,query=g_strdup_printf("SELECT MIN(`%s`),MAX(`%s`) FROM `%s`.`%s`", field, field, database, table));
	g_free(query);
	minmax=mysql_store_result(conn);
	
	if (!minmax)
		goto cleanup;

	row=mysql_fetch_row(minmax);
	MYSQL_FIELD * fields=mysql_fetch_fields(minmax);
	char *min=row[0];
	char *max=row[1];

	/* Got total number of rows, skip chunk logic if estimates are low */
	guint64 rows = estimate_count(conn, database, table, field, NULL, NULL);
	if (rows <= rows_per_file)
		goto cleanup;

	/* This is estimate, not to use as guarantee! Every chunk would have eventual adjustments */
	guint64 estimated_chunks = rows / rows_per_file;
	guint64 estimated_step, nmin, nmax, cutoff;
	int showed_nulls=0;

	/* Support just bigger INTs for now, very dumb, no verify approach */
	switch (fields[0].type) {
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_LONGLONG:
		case MYSQL_TYPE_INT24:
			/* static stepping */
			nmin = strtoll(min,NULL,10);
			nmax = strtoll(max,NULL,10);
			estimated_step = (nmax-nmin)/estimated_chunks+1;
			cutoff = nmin;
			while(cutoff<=nmax) {
				chunks=g_list_append(chunks,g_strdup_printf("%s%s(`%s` >= %llu AND `%s` < %llu)",
						!showed_nulls?field:"",
						!showed_nulls?" IS NULL OR ":"",
						field, (unsigned long long)cutoff,
						field, (unsigned long long)cutoff+estimated_step));
				cutoff+=estimated_step;
				showed_nulls=1;
			}

		default:
			goto cleanup;
	}

cleanup:	
	if (indexes) 
		mysql_free_result(indexes);
	if (minmax)
		mysql_free_result(minmax);
	if (total)
		mysql_free_result(total);

	return chunks;
}

/* Try to get EXPLAIN'ed estimates of row in resultset */
guint64 estimate_count(MYSQL *conn, char *database, char *table, char *field, char *from, char *to) {
	char *querybase, *query;
	int ret;

	g_assert(conn && database && table);

	querybase = g_strdup_printf("EXPLAIN SELECT `%s` FROM `%s`.`%s`", (field?field:"*"), database, table);
	if (from || to) {
		g_assert(field != NULL);
		char *fromclause=NULL, *toclause=NULL;
		char *escaped;
		if (from) {
			escaped=g_new(char,strlen(from)*2+1);
			mysql_real_escape_string(conn,escaped,from,strlen(from));
			fromclause = g_strdup_printf(" `%s` >= \"%s\" ", field, escaped);
			g_free(escaped);
		}
		if (to) {
			escaped=g_new(char,strlen(to)*2+1);
			mysql_real_escape_string(conn,escaped,from,strlen(from));
			toclause = g_strdup_printf( " `%s` <= \"%s\"", field, escaped);
			g_free(escaped);
		}
		query = g_strdup_printf("%s WHERE `%s` %s %s", querybase, (from?fromclause:""), ((from&&to)?"AND":""), (to?toclause:""));

		if (toclause) g_free(toclause);
		if (fromclause) g_free(fromclause);
		ret=mysql_query(conn,query);
		g_free(querybase);
		g_free(query);
	} else {
		ret=mysql_query(conn,querybase);
		g_free(querybase);
	}

	if (ret) {
		g_warning("Unable to get estimates for %s.%s: %s",database,table,mysql_error(conn));
	}

	MYSQL_RES *result = mysql_store_result(conn);
	MYSQL_FIELD *fields = mysql_fetch_fields(result);
	
	int i;
	for (i=0; i<mysql_num_fields(result); i++) {
		if (!strcmp(fields[i].name,"rows"))
			break;
	}

	MYSQL_ROW row = NULL;

	guint64 count=0;

	if (result)
		row = mysql_fetch_row(result);

	if (row && row[i])
		count=strtoll(row[i],NULL,10);

	if (result)
		mysql_free_result(result);

	return(count);
}

void create_backup_dir(char *directory) {
	if (g_mkdir(directory, 0700) == -1)
	{
		if (errno != EEXIST)
		{
			g_critical("Unable to create `%s': %s",
				directory,
				g_strerror(errno));
			exit(1);
		}
	}
}

void dump_database(MYSQL * conn, char *database, struct configuration *conf) {
	mysql_select_db(conn,database);
	if (mysql_query(conn, "SHOW /*!50000 FULL */ TABLES")) {
		g_critical("Error: DB: %s - Could not execute query: %s", database, mysql_error(conn));
		return;
	}
	MYSQL_RES *result = mysql_store_result(conn);
	guint num_fields = mysql_num_fields(result);

	MYSQL_ROW row;
	while ((row = mysql_fetch_row(result))) {
		/* We no care about views! */
		if (num_fields>1 && strcmp(row[1],"BASE TABLE"))
			continue;
		dump_table(conn, database, row[0], conf);
	}
	mysql_free_result(result);
}

void dump_table_data_file(MYSQL *conn, char *database, char *table, char *where, char *filename, struct configuration *conf)
{
	void *outfile;
	
	if (!compress_output)
		outfile = g_fopen(filename, "w");
	else
		outfile = gzopen(filename, "w");

	if (!outfile) {
		g_critical("Error: DB: %s TABLE: %s Could not create output file %s (%d)", database, table, filename, errno);
		return;
	}
	dump_table_data(conn, outfile, database, table, where, conf);
	if (!compress_output)
		fclose(outfile);
	else
		gzclose(outfile);
}

void dump_table(MYSQL *conn, char *database, char *table, struct configuration *conf) {

	GList * chunks = NULL; 

	if (rows_per_file)
		chunks = get_chunks_for_table(conn, database, table, conf);

	if (chunks) {
		int nchunk = 0;
		for (chunks = g_list_first(chunks); chunks; chunks=g_list_next(chunks)) {
			struct job *j = g_new0(struct job, 1);
			j->database=g_strdup(database);
			j->table=g_strdup(table);
			j->conf=conf;
			j->type=JOB_DUMP;
			j->filename=g_strdup_printf("%s/%s.%s.%05d.sql%s", directory, database, table, nchunk,(compress_output?".gz":""));
			j->where=(char *)chunks->data;
			g_async_queue_push(conf->queue,j);
			nchunk++;
		}
		g_list_free(g_list_first(chunks));
	} else {
		struct job *j = g_new0(struct job,1);
		j->database=g_strdup(database);
		j->table=g_strdup(table);
		j->conf=conf;
		j->type=JOB_DUMP;
		j->filename=g_strdup_printf("%s/%s.%s.sql%s", directory, database, table, (compress_output?".gz":""));
		g_async_queue_push(conf->queue,j);
		return;
	}
}

/* Do actual data chunk reading/writing magic */
void dump_table_data(MYSQL *conn, FILE *file, char *database, char *table, char *where, struct configuration *conf)
{
	guint i;
	guint num_fields = 0;
	MYSQL_RES *result = NULL;
	char *query = NULL;

	/* Ghm, not sure if this should be statement_size - but default isn't too big for now */	
	GString* statement = g_string_sized_new(statement_size);

	g_string_printf(statement,"/*!40101 SET NAMES binary*/;\n");
	write_data(file, statement);

	/* Poor man's database code */
	query = g_strdup_printf("SELECT * FROM `%s`.`%s` %s %s", database, table, where?"WHERE":"", where?where:"");
	if (mysql_query(conn, query)) {
		g_critical("Error dumping table (%s.%s) data: %s ",database, table, mysql_error(conn));
		g_free(query);
		return;
	}

	result = mysql_use_result(conn);
	num_fields = mysql_num_fields(result);
	MYSQL_FIELD *fields = mysql_fetch_fields(result);

	/* Buffer for escaping field values */
	GString *escaped = g_string_sized_new(3000);

	MYSQL_ROW row;

	g_string_set_size(statement,0);

	/* Poor man's data dump code */
	while ((row = mysql_fetch_row(result))) {
		gulong *lengths = mysql_fetch_lengths(result);

		if (!statement->len)
			g_string_printf(statement, "INSERT INTO `%s` VALUES\n (", table);
		else
			g_string_append(statement, ",\n (");

		for (i = 0; i < num_fields; i++) {
			/* Don't escape safe formats, saves some time */
			if (!row[i]) {
				g_string_append(statement, "NULL");
			} else if (fields[i].flags & NUM_FLAG) {
				g_string_printf(statement, "\"%s\"", row[i]);
			} else {
				/* We reuse buffers for string escaping, growing is expensive just at the beginning */
				g_string_set_size(escaped, lengths[i]*2+1);
				mysql_real_escape_string(conn, escaped->str, row[i], lengths[i]);
				g_string_append(statement,"\"");
				g_string_append(statement,escaped->str);
				g_string_append(statement,"\"");

			}
			if (i < num_fields - 1) {
				g_string_append(statement, ",");
			} else {
				/* INSERT statement is closed once over limit */
				if (statement->len > statement_size) {
					g_string_append(statement,");\n");
					write_data(file,statement);
					g_string_set_size(statement,0);

				} else {
					g_string_append(statement, ")");
				}
			}
		}
	}
	g_string_printf(statement,";\n");
	write_data(file, statement);
	// cleanup:
	g_free(query);

	g_string_free(escaped,1);
	g_string_free(statement,1);

	if (result) {
		mysql_free_result(result);
	}
}

int write_data(void *file, GString *data)
{
	if (!compress_output)
		return write(fileno(file),data->str,data->len);
	else
		return gzwrite((gzFile)file,data->str,data->len);
}

int write_compressed_data (gzFile file, char *buf, const char *format, va_list va)
{
    int len;
    len = vsnprintf(buf, sizeof(buf), format, va);
    if (len <= 0 || len >= (int)sizeof(buf) || buf[sizeof(buf) - 1] != 0)
        return 0;
    return gzwrite(file, buf, (unsigned)len);
}

