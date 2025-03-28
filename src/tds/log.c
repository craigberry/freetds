/* FreeTDS - Library of routines accessing Sybase and Microsoft databases
 * Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005  Brian Bruns
 * Copyright (C) 2006-2015  Frediano Ziglio
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <config.h>

#include <stdarg.h>

#include <freetds/time.h>

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif /* HAVE_STDLIB_H */

#if HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */

#if HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#ifdef _WIN32
# include <process.h>
#endif

#include <freetds/tds.h>
#include <freetds/checks.h>
#include <freetds/thread.h>
#include <freetds/utils.h>

/* for now all messages go to the log */
int tds_debug_flags = TDS_DBGFLAG_ALL | TDS_DBGFLAG_SOURCE;
int tds_append_mode = 0;
static tds_dir_char *g_dump_filename = NULL;
/** Tell if TDS debug logging is turned on or off */
bool tds_write_dump = false;
/** List of threads excluded from logging, used to exclude some sensitive data */
static TDSDUMP_OFF_ITEM *off_list;
static FILE *g_dumpfile = NULL;	/* file pointer for dump log          */
static tds_mutex g_dump_mutex = TDS_MUTEX_INITIALIZER;

static FILE* tdsdump_append(void);

#ifdef TDS_ATTRIBUTE_DESTRUCTOR
static void __attribute__((destructor))
tds_util_deinit(void)
{
	tdsdump_close();
}
#endif

/**
 * Temporarily turn off logging for current thread.
 * @param off_item  List item to be used by the function.
 *                  The item will be initialized by the function.
 *                  It's retained till is removed with tdsdump_on so it must be kept alive.
 */
void
tdsdump_off(TDSDUMP_OFF_ITEM *off_item)
{
	/* if already off don't add current thread to exclude list to make it faster */
	if (!tds_write_dump)
		return;

	off_item->thread_id = tds_thread_get_current_id();
	tds_mutex_lock(&g_dump_mutex);
	off_item->next = off_list;
	off_list = off_item;
	tds_mutex_unlock(&g_dump_mutex);
}				/* tdsdump_off()  */


/**
 * Turn logging back on for current thread.
 * @param off_item  List item to remove from global list.
 *                  Previously used by tdsdump_off().
 */
void
tdsdump_on(TDSDUMP_OFF_ITEM *off_item)
{
	TDSDUMP_OFF_ITEM **curr;

	tds_mutex_lock(&g_dump_mutex);
	for (curr = &off_list; *curr != NULL; curr = &(*curr)->next) {
		if (*curr == off_item) {
			*curr = (*curr)->next;
			break;
		}
	}
	tds_mutex_unlock(&g_dump_mutex);
}

int
tdsdump_isopen(void)
{
	return g_dumpfile || g_dump_filename;
}


/**
 * Create and truncate a human readable dump file for the TDS
 * traffic.  The name of the file is specified by the filename
 * parameter.  If that is given as NULL or an empty string,
 * any existing log file will be closed.
 *
 * \return  true if the file was opened, false if it couldn't be opened.
 */
int
tdsdump_open(const tds_dir_char *filename)
{
	int result;		/* really should be a boolean, not an int */

	tds_mutex_lock(&g_dump_mutex);

	/* same append file */
	if (tds_append_mode && filename != NULL && g_dump_filename != NULL && tds_dir_cmp(filename, g_dump_filename) == 0) {
		tds_mutex_unlock(&g_dump_mutex);
		return 1;
	}

	tds_write_dump = false;

	/* free old one */
	if (g_dumpfile != NULL && g_dumpfile != stdout && g_dumpfile != stderr)
		fclose(g_dumpfile);
	g_dumpfile = NULL;
	if (g_dump_filename)
		TDS_ZERO_FREE(g_dump_filename);

	/* required to close just log ?? */
	if (filename == NULL || filename[0] == '\0') {
		tds_mutex_unlock(&g_dump_mutex);
		return 1;
	}

	result = 1;
	if (tds_append_mode) {
		g_dump_filename = tds_dir_dup(filename);
		/* if mutex are available do not reopen file every time */
#ifdef TDS_HAVE_MUTEX
		g_dumpfile = tdsdump_append();
#endif
	} else if (!tds_dir_cmp(filename, TDS_DIR("stdout"))) {
		g_dumpfile = stdout;
	} else if (!tds_dir_cmp(filename, TDS_DIR("stderr"))) {
		g_dumpfile = stderr;
	} else if (NULL == (g_dumpfile = tds_dir_open(filename, TDS_DIR("w")))) {
		result = 0;
	}

	if (result)
		tds_write_dump = true;
	tds_mutex_unlock(&g_dump_mutex);

	if (result) {
		char today[64];
		struct tm res;
		time_t t;

		time(&t);
		today[0] = 0;
		if (tds_localtime_r(&t, &res))
			strftime(today, sizeof(today), "%Y-%m-%d %H:%M:%S", &res);

		tdsdump_log(TDS_DBG_INFO1, "Starting log file for FreeTDS %s\n"
			    "\ton %s with debug flags 0x%x.\n", VERSION, today, tds_debug_flags);
	}
	return result;
}				/* tdsdump_open()  */

static FILE*
tdsdump_append(void)
{
	if (!g_dump_filename)
		return NULL;

	if (!tds_dir_cmp(g_dump_filename, TDS_DIR("stdout"))) {
		return stdout;
	} else if (!tds_dir_cmp(g_dump_filename, TDS_DIR("stderr"))) {
		return stderr;
	}
	return tds_dir_open(g_dump_filename, TDS_DIR("a"));
}


/**
 * Close the TDS dump log file.
 */
void
tdsdump_close(void)
{
	tds_mutex_lock(&g_dump_mutex);
	tds_write_dump = false;
	if (g_dumpfile != NULL && g_dumpfile != stdout && g_dumpfile != stderr)
		fclose(g_dumpfile);
	g_dumpfile = NULL;
	if (g_dump_filename)
		TDS_ZERO_FREE(g_dump_filename);
	tds_mutex_unlock(&g_dump_mutex);
}				/* tdsdump_close()  */

static void
tdsdump_start(FILE *file, const char *fname, int line)
{
	char buf[128], *pbuf;
	int started = 0;

	/* write always time before log */
	if (tds_debug_flags & TDS_DBGFLAG_TIME) {
		fputs(tds_timestamp_str(buf, sizeof(buf) - 1), file);
		started = 1;
	}

	pbuf = buf;
	if (tds_debug_flags & TDS_DBGFLAG_PID) {
		if (started)
			*pbuf++ = ' ';
		pbuf += sprintf(pbuf, "%d", (int) getpid());
		started = 1;
	}

	if ((tds_debug_flags & TDS_DBGFLAG_SOURCE) && fname && line) {
		const char *p;
		p = strrchr(fname, '/');
		if (p)
			fname = p + 1;
		p = strrchr(fname, '\\');
		if (p)
			fname = p + 1;
		if (started)
			pbuf += sprintf(pbuf, " (%s:%d)", fname, line);
		else
			pbuf += sprintf(pbuf, "%s:%d", fname, line);
		started = 1;
	}
	if (started)
		*pbuf++ = ':';
	*pbuf = 0;
	fputs(buf, file);
}

/**
 * Check if current thread is in the list of excluded threads
 * g_dump_mutex must be held.
 */
static bool
current_thread_is_excluded(void)
{
	TDSDUMP_OFF_ITEM *curr_off;

	tds_mutex_check_owned(&g_dump_mutex);

	for (curr_off = off_list; curr_off; curr_off = curr_off->next)
		if (tds_thread_is_current(curr_off->thread_id))
			return true;

	return false;
}

#undef tdsdump_dump_buf
/**
 * Dump the contents of data into the log file in a human readable format.
 * \param file       source file name
 * \param level_line line and level combined. This and file are automatically computed by
 *                   TDS_DBG_* macros.
 * \param msg        message to print before dump
 * \param buf        buffer to dump
 * \param length     number of bytes in the buffer
 */
void
tdsdump_dump_buf(const char* file, unsigned int level_line, const char *msg, const void *buf, size_t length)
{
	size_t i, j;
#define BYTES_PER_LINE 16
	const unsigned char *data = (const unsigned char *) buf;
	const int debug_lvl = level_line & 15;
	const int line = level_line >> 4;
	char line_buf[BYTES_PER_LINE * 8 + 16], *p;
	FILE *dumpfile;

	if (((tds_debug_flags >> debug_lvl) & 1) == 0 || !tds_write_dump)
		return;

	if (!g_dumpfile && !g_dump_filename)
		return;

	tds_mutex_lock(&g_dump_mutex);

	if (current_thread_is_excluded()) {
		tds_mutex_unlock(&g_dump_mutex);
		return;
	}

	dumpfile = g_dumpfile;
#ifdef TDS_HAVE_MUTEX
	if (tds_append_mode && dumpfile == NULL)
		dumpfile = g_dumpfile = tdsdump_append();
#else
	if (tds_append_mode)
		dumpfile = tdsdump_append();
#endif

	if (dumpfile == NULL) {
		tds_mutex_unlock(&g_dump_mutex);
		return;
	}

	tdsdump_start(dumpfile, file, line);

	fprintf(dumpfile, "%s\n", msg);

	for (i = 0; i < length; i += BYTES_PER_LINE) {
		p = line_buf;
		/*
		 * print the offset as a 4 digit hex number
		 */
		p += sprintf(p, "%04x", ((unsigned int) i) & 0xffffu);

		/*
		 * print each byte in hex
		 */
		for (j = 0; j < BYTES_PER_LINE; j++) {
			if (j == BYTES_PER_LINE / 2)
				*p++ = '-';
			else
				*p++ = ' ';
			if (j + i >= length)
				p += sprintf(p, "  ");
			else
				p += sprintf(p, "%02x", data[i + j]);
		}

		/*
		 * skip over to the ascii dump column
		 */
		p += sprintf(p, " |");

		/*
		 * print each byte in ascii
		 */
		for (j = i; j < length && (j - i) < BYTES_PER_LINE; j++) {
			if (j - i == BYTES_PER_LINE / 2)
				*p++ = ' ';
			p += sprintf(p, "%c", (isprint(data[j])) ? data[j] : '.');
		}
		strcpy(p, "|\n");
		fputs(line_buf, dumpfile);
	}
	fputs("\n", dumpfile);

	fflush(dumpfile);

#ifndef TDS_HAVE_MUTEX
	if (tds_append_mode) {
		if (dumpfile != stdout && dumpfile != stderr)
			fclose(dumpfile);
	}
#endif

	tds_mutex_unlock(&g_dump_mutex);

}				/* tdsdump_dump_buf()  */
#define tdsdump_dump_buf TDSDUMP_BUF_FAST


#undef tdsdump_log
/**
 * Write a message to the debug log.  
 * \param file name of the log file
 * \param level_line kind of detail to be included
 * \param fmt       printf-like format string
 */
void
tdsdump_log(const char* file, unsigned int level_line, const char *fmt, ...)
{
	const int debug_lvl = level_line & 15;
	const int line = level_line >> 4;
	va_list ap;
	FILE *dumpfile;

	if (((tds_debug_flags >> debug_lvl) & 1) == 0 || !tds_write_dump)
		return;

	if (!g_dumpfile && !g_dump_filename)
		return;

	tds_mutex_lock(&g_dump_mutex);

	if (current_thread_is_excluded()) {
		tds_mutex_unlock(&g_dump_mutex);
		return;
	}

	dumpfile = g_dumpfile;
#ifdef TDS_HAVE_MUTEX
	if (tds_append_mode && dumpfile == NULL)
		dumpfile = g_dumpfile = tdsdump_append();
#else
	if (tds_append_mode)
		dumpfile = tdsdump_append();
#endif
	
	if (dumpfile == NULL) { 
		tds_mutex_unlock(&g_dump_mutex);
		return;
	}

	tdsdump_start(dumpfile, file, line);

	va_start(ap, fmt);

	vfprintf(dumpfile, fmt, ap);
	va_end(ap);

	fflush(dumpfile);

#ifndef TDS_HAVE_MUTEX
	if (tds_append_mode) {
		if (dumpfile != stdout && dumpfile != stderr)
			fclose(dumpfile);
	}
#endif
	tds_mutex_unlock(&g_dump_mutex);
}				/* tdsdump_log()  */
#define tdsdump_log TDSDUMP_LOG_FAST


/**
 * Write a column value to the debug log.  
 * \param col column to dump
 */
void
tdsdump_col(const TDSCOLUMN *col)
{
	const char* type_name;
	char* data;
	TDS_SMALLINT type;
	
	assert(col);
	assert(col->column_data);
	
	type_name = tds_prtype(col->column_type);
	type = tds_get_conversion_type(col->column_type, col->column_size);
	
	switch(type) {
	case SYBCHAR: 
	case SYBVARCHAR:
		if (col->column_cur_size >= 0) {
			data = tds_new0(char, 1 + col->column_cur_size);
			if (!data) {
				tdsdump_log(TDS_DBG_FUNC, "no memory to log data for type %s\n", type_name);
				return;
			}
			memcpy(data, col->column_data, col->column_cur_size);
			tdsdump_log(TDS_DBG_FUNC, "type %s has value \"%s\"\n", type_name, data);
			free(data);
		} else {
			tdsdump_log(TDS_DBG_FUNC, "type %s has value NULL\n", type_name);
		}
		break;
	case SYBINT1:
		tdsdump_log(TDS_DBG_FUNC, "type %s has value %d\n", type_name, (int)*(TDS_TINYINT*)col->column_data);
		break;
	case SYBINT2:
		tdsdump_log(TDS_DBG_FUNC, "type %s has value %d\n", type_name, (int)*(TDS_SMALLINT*)col->column_data);
		break;
	case SYBINT4:
		tdsdump_log(TDS_DBG_FUNC, "type %s has value %d\n", type_name, (int)*(TDS_INT*)col->column_data);
		break;
	case SYBREAL:
		tdsdump_log(TDS_DBG_FUNC, "type %s has value %f\n", type_name, (double)*(TDS_REAL*)col->column_data);
		break;
	case SYBFLT8:
		tdsdump_log(TDS_DBG_FUNC, "type %s has value %f\n", type_name, (double)*(TDS_FLOAT*)col->column_data);
		break;
	default:
		tdsdump_log(TDS_DBG_FUNC, "cannot log data for type %s\n", type_name);
		break;
	}
}
