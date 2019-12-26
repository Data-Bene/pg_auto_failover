/*
 * src/bin/pg_autoctl/pgctl.c
 *   API for controling PostgreSQL, using its binary tooling (pg_ctl,
 *   pg_controldata, pg_basebackup and such).
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "postgres_fe.h"
#include "pqexpbuffer.h"

#include "defaults.h"
#include "file_utils.h"
#include "parsing.h"
#include "pgctl.h"
#include "pgsql.h"
#include "log.h"

#define RUN_PROGRAM_IMPLEMENTATION
#include "runprogram.h"


#define AUTOCTL_DEFAULTS_CONF_FILENAME "postgresql-auto-failover.conf"
#define AUTOCTL_CONF_INCLUDE_LINE "include '" AUTOCTL_DEFAULTS_CONF_FILENAME "'"
#define AUTOCTL_CONF_INCLUDE_REGEX "^include 'postgresql-auto-failover\\.conf'.*"
#define AUTOCTL_CONF_INCLUDE_COMMENT " # Auto-generated by pg_auto_failover, do not remove\n"

#define AUTOCTL_STANDBY_CONF_FILENAME "postgresql-auto-failover-standby.conf"
#define AUTOCTL_SB_CONF_INCLUDE_LINE "include '" AUTOCTL_STANDBY_CONF_FILENAME "'"
#define AUTOCTL_SB_CONF_INCLUDE_REGEX "^include 'postgresql-auto-failover-standby\\.conf'.*"

#define PROGRAM_NOT_RUNNING 3


static bool pg_include_config(const char *configFilePath,
							  const char *configIncludeLine,
							  const char *configIncludeRegex,
							  const char *configIncludeComment);
static bool ensure_default_settings_file_exists(const char *configFilePath,
												GUC *settings,
												PostgresSetup *pgSetup);
static void log_program_output(Program prog);
static bool escape_recovery_conf_string(char *destination,
										int destinationSize,
										const char *recoveryConfString);
static bool prepare_primary_conninfo(char *primaryConnInfo,
									 int primaryConnInfoSize,
									 const char *primaryHost, int primaryPort,
									 const char *replicationUsername,
									 const char *replicationPassword);
static bool pg_write_recovery_conf(const char *pgdata,
								   const char *primaryConnInfo,
								   const char *replicationSlotName);
static bool pg_write_standby_signal(const char *configFilePath,
									const char *pgdata,
									const char *primaryConnInfo,
									const char *replicationSlotName);


/*
 * Get pg_ctl --version output.
 *
 * The caller should free the return value if not NULL.
 */
char *
pg_ctl_version(const char *pg_ctl_path)
{
	char *version;
	Program prog = run_program(pg_ctl_path, "--version", NULL);

	if (prog.returnCode != 0)
	{
		log_error("Failed to run \"pg_ctl --version\" using program \"%s\": %s",
				  pg_ctl_path, strerror(prog.error));
		free_program(&prog);
		return NULL;
	}

	version = parse_version_number(prog.stdout);
	free_program(&prog);

	return version;
}


/*
 * Read some of the information from pg_controldata output.
 */
bool
pg_controldata(PostgresSetup *pgSetup, bool missing_ok)
{
	char pg_controldata_path[MAXPGPATH];
	Program prog;

	if (pgSetup->pgdata[0] == '\0' || pgSetup->pg_ctl[0] == '\0')
	{
		log_debug("Failed to run pg_control_data on an empty pgSetup");
		return false;
	}

	path_in_same_directory(pgSetup->pg_ctl, "pg_controldata", pg_controldata_path);
	log_debug("%s %s", pg_controldata_path, pgSetup->pgdata);

	/* We parse the output of pg_controldata, make sure it's as expected */
	setenv("LANG", "C", 1);
	prog = run_program(pg_controldata_path, pgSetup->pgdata, NULL);

	if (prog.returnCode == 0)
	{
		if (prog.stdout == NULL)
		{
			/* happens sometimes, and I don't know why */
			log_warn("Got empty output from `%s %s`, trying again in 1s",
					 pg_controldata_path, pgSetup->pgdata);
			sleep(1);

			return pg_controldata(pgSetup, missing_ok);
		}

		if (!parse_controldata(&pgSetup->control, prog.stdout))
		{
			log_error("%s %s", pg_controldata_path, pgSetup->pgdata);
			log_warn("Failed to parse pg_controldata output:\n%s", prog.stdout);
			free_program(&prog);
			return false;
		}

		free_program(&prog);
		return true;
	}
	else
	{
		if (!missing_ok)
		{
			char *errorLines[BUFSIZE];
			int lineCount = splitLines(prog.stderr, errorLines, BUFSIZE);
			int lineNumber = 0;

			for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
			{
				/*
				 * pg_controldata typically errors out a single line prefixed
				 * with the name of the binary.
				 */
				log_error("%s", errorLines[lineNumber]);
			}
			log_error("Failed to run \"%s\" on \"%s\", see above for details",
					  pg_controldata_path, pgSetup->pgdata);
		}
		free_program(&prog);

		return missing_ok;
	}
}


/*
 * Find "pg_ctl" programs in the PATH. If a single one exists, set its absolute
 * location in pg_ctl, and the PostgreSQL version number in pg_version.
 *
 * Returns how many "pg_ctl" programs have been found in the PATH.
 */
int
config_find_pg_ctl(PostgresSetup *pgSetup)
{
	char **pg_ctls = NULL;
	int n = search_pathlist(getenv("PATH"), "pg_ctl", &pg_ctls);

	pgSetup->pg_ctl[0] = '\0';
	pgSetup->pg_version[0] = '\0';

	if (n == 1)
	{
		char *program = pg_ctls[0];
		char *version = pg_ctl_version(program);

		log_info("Found pg_ctl for PostgreSQL %s at %s", version, program);

		strlcpy(pgSetup->pg_ctl, program, MAXPGPATH);
		strlcpy(pgSetup->pg_version, version, PG_VERSION_STRING_MAX);

		free(version);
	}
	else if (n == 0)
	{
		log_warn("Failed to find pg_ctl in PATH");
	}
	else
	{
		for (int i = 0; i < n; i++)
		{
			char *program = pg_ctls[i];
			char *version = pg_ctl_version(program);

			log_info("Found %s for pg version %s", program, version);
			free(version);
		}
	}

	search_pathlist_destroy_result(pg_ctls);

	return n;
}


/*
 * pg_add_auto_failover_default_settings ensures the pg_auto_failover default
 * settings are included in postgresql.conf. For simplicity, this function
 * reads the whole contents of postgresql.conf into memory.
 */
bool
pg_add_auto_failover_default_settings(PostgresSetup *pgSetup,
									  char *configFilePath,
									  GUC *settings)
{
	char pgAutoFailoverDefaultsConfigPath[MAXPGPATH];

	/*
	 * Write the default settings to postgresql-auto-failover.conf.
	 *
	 * postgresql-auto-failover.conf needs to be placed alongside
	 * postgresql.conf for the include to work. Determine the path by finding
	 * the parent directory of postgresql.conf.
	 */
	path_in_same_directory(configFilePath, AUTOCTL_DEFAULTS_CONF_FILENAME,
						   pgAutoFailoverDefaultsConfigPath);

	if (!ensure_default_settings_file_exists(pgAutoFailoverDefaultsConfigPath,
											 settings, pgSetup))
	{
		return false;
	}

	return pg_include_config(configFilePath,
							 AUTOCTL_CONF_INCLUDE_LINE,
							 AUTOCTL_CONF_INCLUDE_REGEX,
							 AUTOCTL_CONF_INCLUDE_COMMENT);
}


/*
 * pg_include_config adds an include line to postgresql.conf to include the
 * given configuration file, with a comment refering pg_auto_failover.
 */
static bool
pg_include_config(const char *configFilePath,
				  const char *configIncludeLine,
				  const char *configIncludeRegex,
				  const char *configIncludeComment)
{
	char *includeLine = NULL;
	char *currentConfContents = NULL;
	long currentConfSize = 0L;
	PQExpBuffer newConfContents = NULL;

	/* read the current postgresql.conf contents */
	if (!read_file(configFilePath, &currentConfContents, &currentConfSize))
	{
		return false;
	}

	/* find the include 'postgresql-auto-failover.conf' line */
	includeLine = regexp_first_match(currentConfContents, configIncludeRegex);
	if (includeLine != NULL)
	{
		log_debug("%s found in \"%s\"", configIncludeLine, configFilePath);

		/* defaults settings are already included */
		free(currentConfContents);
		free(includeLine);
		return true;
	}

	log_debug("Adding %s to \"%s\"", configIncludeLine, configFilePath);

	/* build the new postgresql.conf contents */
	newConfContents = createPQExpBuffer();
	if (newConfContents == NULL)
	{
		log_error("Failed to allocate memory");
		free(currentConfContents);
		return false;
	}

	appendPQExpBufferStr(newConfContents, configIncludeLine);
	appendPQExpBufferStr(newConfContents, configIncludeComment);
	appendPQExpBufferStr(newConfContents, currentConfContents);

	/* done with the old postgresql.conf contents */
	free(currentConfContents);

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(newConfContents))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(newConfContents);
		return false;
	}

	/* write the new postgresql.conf */
	if (!write_file(newConfContents->data, newConfContents->len, configFilePath))
	{
		destroyPQExpBuffer(newConfContents);
		return false;
	}

	destroyPQExpBuffer(newConfContents);

	return true;
}


/*
 * ensure_default_settings_file_exists writes the postgresql-auto-failover.conf
 * file to the database directory.
 */
static bool
ensure_default_settings_file_exists(const char *configFilePath,
									GUC *settings,
									PostgresSetup *pgSetup)
{
	PQExpBuffer defaultConfContents = createPQExpBuffer();
	int settingIndex = 0;

	appendPQExpBufferStr(defaultConfContents, "# Settings by pg_auto_failover\n");

	/* replace placeholder values with actual pgSetup values */
	for (settingIndex = 0; settings[settingIndex].name != NULL; settingIndex++)
	{
		GUC *setting = &settings[settingIndex];
		/*
		 * Settings for "listen_addresses" and "port" are replaced with the
		 * respective values present in pgSetup allowing those to be dynamic.
		 *
		 * At the moment our "needs quote" heuristic is pretty simple.
		 * There's the one parameter within those that we hardcode from
		 * pg_auto_failover that needs quoting, and that's
		 * listen_addresses.
		 *
		 * The reason why POSTGRES_DEFAULT_LISTEN_ADDRESSES is not quoting
		 * the value directly in the constant is that we are using that
		 * value both in the configuration file and at the pg_ctl start
		 * --options "-h *" command line.
		 *
		 * At the command line, using --options "-h '*'" would give:
		 *    could not create listen socket for "'*'"
		 */
		if (strcmp(setting->name, "listen_addresses") == 0)
		{
			appendPQExpBuffer(defaultConfContents, "%s = '%s'\n",
							  setting->name,
							  pgSetup->listen_addresses);
		}
		else if (strcmp(setting->name, "port") == 0)
		{
			appendPQExpBuffer(defaultConfContents, "%s = %d\n",
					  setting->name,
					  pgSetup->pgport);
		}
		else if (setting->value != NULL)
		{
			appendPQExpBuffer(defaultConfContents, "%s = %s\n",
							  setting->name,
							  setting->value);
		}
		else
		{
			log_error("BUG: GUC setting \"%s\" has a NULL value", setting->name);
			destroyPQExpBuffer(defaultConfContents);
			return false;
		}
	}

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(defaultConfContents))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(defaultConfContents);
		return false;
	}

	if (file_exists(configFilePath))
	{
		char *currentDefaultConfContents = NULL;
		long currentDefaultConfSize = 0L;

		if (!read_file(configFilePath, &currentDefaultConfContents,
					   &currentDefaultConfSize))
		{
			/* technically, we could still try writing, but this is pretty
			 * suspicious */
			destroyPQExpBuffer(defaultConfContents);
			return false;
		}

		if (strcmp(currentDefaultConfContents, defaultConfContents->data) == 0)
		{
			/* file is there and has the same contents, nothing to do */
			log_debug("Default settings file \"%s\" exists", configFilePath);
			free(currentDefaultConfContents);
			destroyPQExpBuffer(defaultConfContents);
			return true;
		}

		log_warn("Contents of \"%s\" have changed, overwriting", configFilePath);
		free(currentDefaultConfContents);
	}
	else
	{
		log_debug("Configuration file \"%s\" doesn't exists yet, "
				  "creating with content:\n%s",
				  configFilePath, defaultConfContents->data);
	}

	if (!write_file(defaultConfContents->data, defaultConfContents->len, configFilePath))
	{
		destroyPQExpBuffer(defaultConfContents);
		return false;
	}

	destroyPQExpBuffer(defaultConfContents);

	return true;
}


/*
 * Call pg_basebackup, using a temporary directory for the duration of the data
 * transfer.
 */
bool
pg_basebackup(const char *pgdata,
			  const char *pg_ctl,
			  const char *backupdir,
			  const char *maximum_backup_rate,
			  const char *replication_username,
			  const char *replication_password,
			  const char *replication_slot_name,
			  const char *primary_hostname, int primary_port)
{
	int returnCode;
	Program program;
	char primary_port_str[10];
	char pg_basebackup[MAXPGPATH];

	log_debug("mkdir -p \"%s\"", backupdir);
	if (!ensure_empty_dir(backupdir, 0700))
	{
		/* errors have already been logged. */
		return false;
	}

	/* call pg_basebackup */
	path_in_same_directory(pg_ctl, "pg_basebackup", pg_basebackup);
	snprintf(primary_port_str, sizeof(primary_port_str), "%d", primary_port);
	setenv("PGCONNECT_TIMEOUT", POSTGRES_CONNECT_TIMEOUT, 1);
	if (replication_password != NULL)
	{
		setenv("PGPASSWORD", replication_password, 1);
	}
	log_info("Running %s -w -h %s -p %d --pgdata %s -U %s --write-recovery-conf "
			 "--max-rate %s --wal-method=stream --slot %s ...",
			 pg_basebackup, primary_hostname, primary_port, backupdir,
			 replication_username, maximum_backup_rate,
			 replication_slot_name);
	program = run_program(pg_basebackup,
						  "-w",
						  "-h", primary_hostname,
						  "-p", primary_port_str,
						  "--pgdata", backupdir,
						  "-U", replication_username,
						  "--verbose",
						  "--progress",
						  "--write-recovery-conf",
						  "--max-rate", maximum_backup_rate,
						  "--wal-method=stream",
						  "--slot", replication_slot_name,
						  NULL);

	log_program_output(program);
	returnCode = program.returnCode;
	free_program(&program);

	if (returnCode != 0)
	{
		log_error("Failed to run pg_basebackup: exit code %d", returnCode);
		return false;
	}

	/* replace $pgdata with the backup directory */
	if (directory_exists(pgdata))
	{
		if (!rmtree(pgdata, true))
		{
			log_error("Failed to remove directory \"%s\": %s",
					  pgdata, strerror(errno));
			return false;
		}
	}

	log_debug("mv \"%s\" \"%s\"", backupdir, pgdata);

	if (rename(backupdir, pgdata) != 0)
	{
		log_error(
			"Failed to install pg_basebackup dir " " \"%s\" in \"%s\": %s",
			backupdir, pgdata, strerror(errno));
		return false;
	}

	return true;
}


/*
 * pg_rewind runs the pg_rewind program to rewind the given database directory
 * to a state where it can follow the given primary. We need the ability to
 * connect to the node.
 */
bool
pg_rewind(const char *pgdata, const char *pg_ctl, const char *primaryHost,
		  int primaryPort, const char *databaseName, const char *replicationUsername,
		  const char *replicationPassword)
{
	int returnCode;
	Program program;
	char primaryConnInfo[MAXCONNINFO] = { 0 };
	char *connInfoEnd = primaryConnInfo;
	char pg_rewind[MAXPGPATH] = { 0 };

	connInfoEnd += make_conninfo_field_str(connInfoEnd, "host", primaryHost);
	connInfoEnd += make_conninfo_field_int(connInfoEnd, "port", primaryPort);
	connInfoEnd += make_conninfo_field_str(connInfoEnd, "user", replicationUsername);
	connInfoEnd += make_conninfo_field_str(connInfoEnd, "dbname", databaseName);

	/* call pg_rewind*/
	path_in_same_directory(pg_ctl, "pg_rewind", pg_rewind);

	setenv("PGCONNECT_TIMEOUT", POSTGRES_CONNECT_TIMEOUT, 1);

	if (replicationPassword != NULL)
	{
		setenv("PGPASSWORD", replicationPassword, 1);
	}

	log_info("Running %s --target-pgdata \"%s\" --source-server \"%s\" --progress ...",
			 pg_rewind, pgdata, primaryConnInfo);

	program = run_program(pg_rewind,
						  "--target-pgdata", pgdata,
						  "--source-server", primaryConnInfo,
						  "--progress",
						  NULL);

	log_program_output(program);
	returnCode = program.returnCode;
	free_program(&program);

	if (returnCode != 0)
	{
		log_error("Failed to run pg_rewind: exit code %d", returnCode);
		return false;
	}

	return true;
}


/* log_program_output logs the output of the given program. */
static void
log_program_output(Program prog)
{
	if (prog.stdout != NULL)
	{
		log_info("%s", prog.stdout);
	}

	if (prog.stderr != NULL)
	{
		if (prog.returnCode == 0)
		{
			log_info("%s", prog.stderr);
		}
		else
		{
			log_error("%s", prog.stderr);
		}
	}
}


/*
 * pg_ctl_initdb initialises a PostgreSQL directory from scratch by calling
 * "pg_ctl initdb", and returns true when this was successful. Beware that it
 * will inherit from the environment, such as LC_COLLATE and LC_ALL etc.
 *
 * No provision is made to control (sanitize?) that environment.
 */
bool
pg_ctl_initdb(const char *pg_ctl, const char *pgdata)
{
	Program program = run_program(pg_ctl, "initdb", "-s", "-D", pgdata, NULL);
	int returnCode = program.returnCode;

	log_info("Initialising a PostgreSQL cluster at \"%s\"", pgdata);
	log_debug("%s initdb -s -D %s [%d]", pg_ctl, pgdata, returnCode);

	if (returnCode != 0)
	{
		log_program_output(program);
	}
	free_program(&program);

	return returnCode == 0;
}


/*
 * pg_ctl_start tries to start a PostgreSQL server by running a "pg_ctl start"
 * command. If the server was started successfully, it returns true.
 */
bool
pg_ctl_start(const char *pg_ctl,
			 const char *pgdata, int pgport, char *listen_addresses)
{
	bool success = false;
	Program program;
	char logfile[MAXPGPATH];
	char pgport_option[20];
	char listen_addresses_option[BUFSIZE];
	char option_unix_socket_directory[BUFSIZE];

	char *args[12];
	int argsIndex = 0;

	char command[BUFSIZE];
	int commandSize = 0;

	join_path_components(logfile, pgdata, "startup.log");
	snprintf(pgport_option, sizeof(pgport_option), "\"-p %d\"", pgport);

	args[argsIndex++] = (char *) pg_ctl;
	args[argsIndex++] = "--pgdata";
	args[argsIndex++] = (char *) pgdata;
	args[argsIndex++] = "--options";
	args[argsIndex++] = (char *) pgport_option;

	if (!IS_EMPTY_STRING_BUFFER(listen_addresses))
	{
		snprintf(listen_addresses_option, sizeof(listen_addresses_option),
				 "\"-h %s\"", listen_addresses);

		args[argsIndex++] = "--options";
		args[argsIndex++] = (char *) listen_addresses_option;
	}

	if (getenv("PG_REGRESS_SOCK_DIR") != NULL)
	{
		snprintf(option_unix_socket_directory,
				 sizeof(option_unix_socket_directory),
				 "\"-k \"%s\"\"",
				 getenv("PG_REGRESS_SOCK_DIR"));

		/* pg_ctl --options can be specified multiple times */
		args[argsIndex++] = "--options";
		args[argsIndex++] = option_unix_socket_directory;
	}

	args[argsIndex++] = "--wait";
	args[argsIndex++] = "start";
	args[argsIndex] = NULL;

	/* we want to call setsid() when running this program. */
	program = initialize_program(args, true);

	/* log the exact command line we're using */
	commandSize = snprintf_program_command_line(&program, command, BUFSIZE);

	if (commandSize >= BUFSIZE)
	{
		/* we only display the first BUFSIZE bytes of the real command */
		log_info("%s...", command);
	}
	else
	{
		log_info("%s", command);
	}

	(void) execute_program(&program);

	if (program.returnCode != 0)
	{
		/*
		 * The command `pg_ctl start` returns a non-zero return code when the
		 * PostgreSQL is already running, because in that case it failed to
		 * start it:
		 *
		 * pg_ctl: another server might be running; trying to start server
		 * anyway HINT: Is another postmaster (PID 15841) running in data
		 * directory "..."?
		 *
		 * That PostgreSQL is currently running is a sign of success condition
		 * for pg_ctl_start, though.
		 */
		Program statusProgram = run_program(pg_ctl, "status", "-D", pgdata, NULL);
		int statusReturnCode = statusProgram.returnCode;

		if (statusReturnCode == 0)
		{
			/* PostgreSQL is running. */
			success = true;

			/* pg_ctl start output is known to be all on stdout. */
			log_warn("Failed to start PostgreSQL. pg_ctl start returned: %d",
					 program.returnCode);

			if (program.stdout != NULL)
			{
				log_warn("%s", program.stdout);
			}

			log_info("PostgreSQL is running. pg_ctl status returned %d",
					 statusReturnCode);
			log_program_output(statusProgram);
		}
		else
		{
			success = false;

			log_error("Failed to start PostgreSQL. pg_ctl start returned: %d",
					  program.returnCode);

			if (program.stdout)
			{
				log_error("%s", program.stdout);
			}
		}

		free_program(&statusProgram);
	}
	else
	{
		success = true;
	}

	/*
	 * Now append the output from pg_ctl start (known to be all in stdout) to
	 * the startup log file, as if by using pg_ctl --log option.
	 */
	if (program.stdout)
	{
		append_to_file(program.stdout, strlen(program.stdout), logfile);
	}

	free_program(&program);

	return success;
}


/*
 * pg_ctl_stop tries to stop a PostgreSQL server by running a "pg_ctl stop"
 * command. If the server was stopped successfully, or if the server is not
 * running at all, it returns true.
 */
bool
pg_ctl_stop(const char *pg_ctl, const char *pgdata)
{
	Program program;
	int status = 0;
	bool pgdata_exists = false;
	const bool log_output = true;

	log_debug("%s --pgdata %s --wait stop --mode fast", pg_ctl, pgdata);

	program = run_program(pg_ctl,
						  "--pgdata", pgdata,
						  "--wait",
						  "stop",
						  "--mode", "fast",
						  NULL);

	/*
	 * Case 1. "pg_ctl stop" was successful, so we could stop the PostgreSQL
	 * server successfully.
	 */
	if (program.returnCode == 0)
	{
		free_program(&program);
		return true;
	}

	/*
	 * Case 2. The data directory doesn't exist. So we assume PostgreSQL is
	 * not running, so stopping the PostgreSQL server was successful.
	 */
	pgdata_exists = directory_exists(pgdata);
	if (!pgdata_exists)
	{
		log_info("pgdata \"%s\" does not exists, consider this as PostgreSQL "
				 "not running", pgdata);
		free_program(&program);
		return true;
	}

	/*
	 * Case 3. "pg_ctl stop" returns non-zero return code when PostgreSQL is not
	 * running at all. So we double-check with "pg_ctl status", and return
	 * success if the PostgreSQL server is not running. Otherwise, we return
	 * failure.
	 *
	 * See https://www.postgresql.org/docs/current/static/app-pg-ctl.html
	 */

	status = pg_ctl_status(pg_ctl, pgdata, log_output);
	if (status == PROGRAM_NOT_RUNNING)
	{
		log_info("pg_ctl stop failed, but PostgreSQL is not running anyway");
		free_program(&program);
		return true;
	}

	log_info("Stopping PostgreSQL server failed. pg_ctl status returned: %d", status);

	if (log_output)
	{
		log_program_output(program);
	}

	free_program(&program);
	return false;
}


/*
 * pg_ctl_status gets the status of the PostgreSQL server by running
 * "pg_ctl status". Output of this command is logged if log_output is true.
 * Return code of this command is returned.
 */
int
pg_ctl_status(const char *pg_ctl, const char *pgdata, bool log_output)
{
	Program program = run_program(pg_ctl, "status", "-D", pgdata, NULL);
	int returnCode = program.returnCode;

	log_debug("%s status -D %s [%d]", pg_ctl, pgdata, returnCode);

	if (log_output)
	{
		log_program_output(program);
	}

	free_program(&program);
	return returnCode;
}


/*
 * pg_ctl_restart calls `pg_ctl restart` on our cluster in immediate mode.
 */
bool
pg_ctl_restart(const char *pg_ctl, const char *pgdata)
{
	Program program = run_program(pg_ctl,
								  "restart",
								  "--pgdata", pgdata,
								  "--silent",
								  "--wait",
								  "--mode", "fast",
								  NULL);
	int returnCode = program.returnCode;

	log_debug("%s restart --pgdata %s --silent --wait --mode fast [%d]",
			  pg_ctl, pgdata, returnCode);

	if (returnCode != 0)
	{
		log_program_output(program);
	}
	free_program(&program);

	return returnCode == 0;
}


/*
 * pg_ctl_promote promotes a standby by running "pg_ctl promote"
 */
bool
pg_ctl_promote(const char *pg_ctl, const char *pgdata)
{
	Program program = run_program(pg_ctl, "promote", "-D", pgdata, "-w", NULL);
	int returnCode = program.returnCode;

	log_debug("%s promote -D %s", pg_ctl, pgdata);

	if (program.stderr != NULL)
	{
		log_error("%s", program.stderr);
	}

	if (returnCode != 0)
	{
		/* pg_ctl promote will have logged errors */
		free_program(&program);
		return false;
	}

	free_program(&program);
	return true;
}


/*
 * pg_setup_standby_mode sets up standby mode by either writing a recovery.conf
 * file or adding the configuration items to postgresql.conf and then creating
 * a standby.signal file in PGDATA.
 */
bool
pg_setup_standby_mode(uint32_t pg_control_version,
					  const char *configFilePath,
					  const char *pgdata,
					  ReplicationSource *replicationSource)
{
	NodeAddress *primaryNode = &(replicationSource->primaryNode);
	char primaryConnInfo[MAXCONNINFO] = { 0 };

	/* we ignore the length returned by prepare_primary_conninfo... */
	if (!prepare_primary_conninfo(primaryConnInfo,
								  MAXCONNINFO,
								  primaryNode->host,
								  primaryNode->port,
								  replicationSource->userName,
								  replicationSource->password))
	{
		/* errors have already been logged. */
		return false;
	}

	if (pg_control_version < 1200)
	{
		/*
		 * Before Postgres 12 we used to place recovery configuration in a
		 * specific file recovery.conf, located alongside postgresql.conf.
		 * Controling whether the server would start in PITR or standby mode
		 * was controlled by a setting in the recovery.conf file.
		 */
		return pg_write_recovery_conf(pgdata,
									  primaryConnInfo,
									  replicationSource->slotName);
	}
	else
	{
		/*
		 * Starting in Postgres 12 we need to add our recovery configuration to
		 * the main postgresql.conf file and create an empty standby.signal
		 * file to trigger starting the server in standby mode.
		 */
		return pg_write_standby_signal(configFilePath,
									   pgdata,
									   primaryConnInfo,
									   replicationSource->slotName);
	}
}


/*
 * pg_write_recovery_conf writes a recovery.conf file to a postgres data
 * directory with the given primary connection info and replication slot name.
 */
static bool
pg_write_recovery_conf(const char *pgdata,
					   const char *primaryConnInfo,
					   const char *replicationSlotName)
{
	char recoveryConfPath[MAXPGPATH];
	PQExpBuffer content = NULL;

	log_trace("pg_write_recovery_conf");

	/* build the contents of recovery.conf */
	content = createPQExpBuffer();
	appendPQExpBuffer(content, "standby_mode = 'on'");
	appendPQExpBuffer(content, "\nprimary_conninfo = %s", primaryConnInfo);
	appendPQExpBuffer(content, "\nprimary_slot_name = '%s'", replicationSlotName);
	appendPQExpBuffer(content, "\nrecovery_target_timeline = 'latest'");
	appendPQExpBuffer(content, "\n");

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(content))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(content);
		return false;
	}

	join_path_components(recoveryConfPath, pgdata, "recovery.conf");

	log_info("Writing recovery configuration to \"%s\"", recoveryConfPath);

	if (!write_file(content->data, content->len, recoveryConfPath))
	{
		/* write_file logs I/O error */
		destroyPQExpBuffer(content);
		return false;
	}

	destroyPQExpBuffer(content);
	return true;
}


/*
 * escape_recovery_conf_string escapes a string that is used in a recovery.conf
 * file by converting single quotes into two single quotes.
 *
 * The result is written to destination and the length of the result.
 */
static bool
escape_recovery_conf_string(char *destination, int destinationSize,
							const char *recoveryConfString)
{
	int charIndex = 0;
	int length = strlen(recoveryConfString);
	int escapedStringLength = 0;

	/* we are going to add at least 3 chars: two quotes and a NUL character */
	if (destinationSize < (length+3))
	{
		log_error("BUG: failed to escape recovery parameter value \"%s\" "
				  "in a buffer of %d bytes",
				  recoveryConfString, destinationSize);
		return false;
	}

	destination[escapedStringLength++] = '\'';

	for (charIndex = 0; charIndex < length; charIndex++)
	{
		char currentChar = recoveryConfString[charIndex];

		if (currentChar == '\'')
		{
			destination[escapedStringLength++] = '\'';
			if (destinationSize < escapedStringLength)
			{
				log_error(
					"BUG: failed to escape recovery parameter value \"%s\" "
					"in a buffer of %d bytes, stopped at index %d",
					recoveryConfString, destinationSize, charIndex);
				return false;
			}
		}

		destination[escapedStringLength++] = currentChar;
		if (destinationSize < escapedStringLength)
		{
			log_error("BUG: failed to escape recovery parameter value \"%s\" "
					  "in a buffer of %d bytes, stopped at index %d",
					  recoveryConfString, destinationSize, charIndex);
			return false;
		}
	}

	destination[escapedStringLength++] = '\'';
	destination[escapedStringLength] = '\0';

	return true;
}


/*
 * prepare_primary_conninfo
 */
static bool
prepare_primary_conninfo(char *primaryConnInfo, int primaryConnInfoSize,
						 const char *primaryHost, int primaryPort,
						 const char *replicationUsername,
						 const char *replicationPassword)
{
	int size = 0;
	char escaped[BUFSIZE];
	PQExpBuffer buffer = NULL;

	buffer = createPQExpBuffer();

	appendPQExpBuffer(buffer, "host=%s", primaryHost);
	appendPQExpBuffer(buffer, " port=%d", primaryPort);
	appendPQExpBuffer(buffer, " user=%s", replicationUsername);

	if (replicationPassword != NULL)
	{
		appendPQExpBuffer(buffer, " password=%s", replicationPassword);
	}

	/* memory allocation could have failed while building string */
	if (PQExpBufferBroken(buffer))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(buffer);
		return false;
	}

	if (!escape_recovery_conf_string(escaped, BUFSIZE, buffer->data))
 	{
 		/* errors have already been logged. */
 		destroyPQExpBuffer(buffer);
 		return false;
 	}

	/* now copy the buffer into primaryConnInfo for the caller */
	size = snprintf(primaryConnInfo, primaryConnInfoSize, "%s", escaped);

	if (size == -1 || size > primaryConnInfoSize)
	{
		log_error("BUG: the escaped primary_conninfo requires %d bytes and "
				  "pg_auto_failover only support up to %d bytes",
				  size, primaryConnInfoSize);
		return false;
	}

	destroyPQExpBuffer(buffer);

	return true;
}


/*
 * pg_write_standby_signal writes the ${PGDATA}/standby.signal file that is in
 * use starting with Postgres 12 for starting a standby server. The file only
 * needs to exists, and the setup is to be found in the main Postgres
 * configuration file.
 */
static bool
pg_write_standby_signal(const char *configFilePath,
						const char *pgdata,
						const char *primaryConnInfo,
						const char *replicationSlotName)
{
	GUC standby_settings[] = {
		{ "primary_conninfo", (char  *)primaryConnInfo },
		{ "primary_slot_name", (char  *) replicationSlotName},
		{ "recovery_target_timeline", "latest"},
		{ NULL, NULL }
	};
	char standbyConfigFilePath[MAXPGPATH];
	char signalFilePath[MAXPGPATH];

	log_trace("pg_write_standby_signal");

	/*
	 * First install the standby.signal file, so that if there's a problem
	 * later and Postgres is started, it is started as a standby, with missing
	 * configuration.
	 */
	join_path_components(signalFilePath, pgdata, "standby.signal");

	log_info("Writing recovery configuration to \"%s\"", signalFilePath);

	if (!write_file("", 0, signalFilePath))
	{
		/* write_file logs I/O error */
		return false;
	}

	/*
	 * Now write the standby settings to postgresql-auto-failover-standby.conf
	 * and include that file from postgresql.conf.
	 */
	path_in_same_directory(configFilePath, AUTOCTL_STANDBY_CONF_FILENAME,
						   standbyConfigFilePath);

	/* we pass NULL as pgSetup because we know it won't be used... */
	if (!ensure_default_settings_file_exists(standbyConfigFilePath,
											 standby_settings,
											 NULL))
	{
		return false;
	}

	/*
	 * We successfully created the standby.signal file, so Postgres will start
	 * as a standby. If we fail to install the standby settings, then we return
	 * false here and let the main loop try again. At least Postgres won't
	 * start as a cloned single accepting writes.
	 */
	if (!pg_include_config(standbyConfigFilePath,
						   AUTOCTL_SB_CONF_INCLUDE_LINE,
						   AUTOCTL_SB_CONF_INCLUDE_REGEX,
						   AUTOCTL_CONF_INCLUDE_COMMENT))
	{
		log_error("Failed to prepare \"%s\" with standby settings",
				  standbyConfigFilePath);
		return false;
	}

	return true;
}


/*
 * pg_is_running returns true if PostgreSQL is running.
 */
bool
pg_is_running(const char *pg_ctl, const char *pgdata)
{
	return pg_ctl_status(pg_ctl, pgdata, false) == 0;
}
