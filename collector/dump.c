/*
 * grubbing about in another process to dump its
 * buffers, and a suitable header etc.
 */
#include "common.h"

#include <sys/mman.h>
#include <sys/klog.h>
#include <sys/utsname.h>

/*
 * Dump kernel dmesg log for kernel init charting.
 */
int
dump_dmsg (const char *output_path)
{
	int size, i, count;
	char *logbuf = NULL;
	char fname[4096];
	FILE *dmesg;

	for (size = 256 * 1024;; size *= 2) {
		logbuf = (char *)realloc (logbuf, size);
		count = klogctl (3, logbuf, size);
		if (count < size - 1)
			break;
	}

	if (!count) {
		free(logbuf);
		log (" odd - no dmesg log data\n");
		return 1;
	}
	
	logbuf[count] = '\0';

	snprintf (fname, 4095, "%s/dmesg", output_path);
	dmesg = fopen (fname, "w");
	if (!dmesg) {
		free(logbuf);
		return 1;
	}

	for (i = 0; i < count; i++) {

		/* skip log level header '<2>...' eg. */
		while (i < count && logbuf[i] != '>') i++;
		i++;

		/* drop line to disk */
		while (i < count && logbuf[i - 1] != '\n')
			fputc (logbuf[i++], dmesg);
	}
	if (logbuf[count - 1] != '\n')
		fputs ("\n", dmesg);

	fclose (dmesg);
	free(logbuf);
	return 0;
}

/* sane ASCII chars only please */
static void
rewrite_ascii (char *string)
{
	char *p;
	for (p = string; *p; p++) {
		if (!isgraph (*p) && !isblank (*p))
			*p = '.';
	}
}

int
dump_header (const char *output_path)
{
	FILE *header;
	char fname[4096];

	if (output_path) {
		snprintf (fname, 4095, "%s/header", output_path);
		header = fopen (fname, "w");
	} else
		header = stdout;

	if (!header)
		return 1;

	fprintf (header, "version = " VERSION "\n");

	{
		time_t now;
		char host_buf[4096] = { '\0' };
		char domain_buf[2048] = { '\0' };
		char time_buf[128];

		if (!gethostname (host_buf, 2047) &&
		    !getdomainname (domain_buf, 2048)) {
			if (strlen (domain_buf)) {
				strcat (host_buf, ".");
				strcat (host_buf, domain_buf);
			}
		} else
			strcpy (host_buf, "unknown");

		rewrite_ascii (host_buf);

		now = time (NULL);
		ctime_r (&now, time_buf);
		if (strrchr (time_buf, '\n'))
			*strrchr (time_buf, '\n') = '\0';

		fprintf (header, "title = Boot chart for %s (%s)\n", host_buf, time_buf);
	}

	{
		struct utsname ubuf;
		if (!uname (&ubuf))
			fprintf (header, "system.uname = %s %s %s %s\n",
				 ubuf.sysname, ubuf.release, ubuf.version, ubuf.machine);
	}
	{
		FILE *lsb;
		char release[4096] = "";

		lsb = popen ("lsb_release -sd", "r");
		if (lsb && fgets (release, 4096, lsb)) {
			if (release[0] == '"')
				memmove (release, release + 1, strlen (release + 1));
			if (strrchr (release, '"'))
				*strrchr (release, '"') = '\0';
		} else
			release[0] = '\0';
		fprintf (header, "system.release = %s\n", release);
		if (lsb)
			pclose (lsb);
	}

	{
		FILE *cpuinfo = fopen ("/proc/cpuinfo", "r");
		FILE *cpuinfo_dump;
		char fname[4096];
		char line[4096];
		char cpu_model[4096] = {'\0'};
		char cpu_model_alt[4096] = {'\0'};
		char *cpu_m = cpu_model;
		int  cpus = 0;

		sprintf (fname, "%s/proc_cpuinfo.log", output_path);
		cpuinfo_dump = fopen(fname, "w");

		/* Dump /proc/cpuinfo for easier debugging with unexpected formats */
		while (cpuinfo && fgets (line, 4096, cpuinfo)) {
			if (!strncmp (line, "model name", 10) && strchr (line, ':'))
				strcpy (cpu_model, strstr (line, ": ") + 2);
			/* ARM platforms save cpu model on Processor field so try to get it */
			if (!strncasecmp (line, "processor", 9)) {
				cpus++;
				strcpy (cpu_model_alt, strstr (line, ": ") + 2);
			}
			if (cpuinfo_dump)
				fprintf(cpuinfo_dump, "%s", line);
		}
		if (cpuinfo)
			fclose (cpuinfo);
		if (cpuinfo_dump)
			fclose(cpuinfo_dump);
		if (!cpu_model[0])
			cpu_m = cpu_model_alt;
		if (strrchr (cpu_m, '\n'))
			*strrchr (cpu_m, '\n') = '\0';
		fprintf (header, "system.cpu = %s %d\n", cpu_m, cpus);
		fprintf (header, "system.cpu.num = %d\n", cpus);
	}
	{
		FILE *cmdline = fopen ("/proc/cmdline", "r");
		if (cmdline) {
			char line [4096] = "";
			assert (NULL != fgets (line, 4096, cmdline));
			fprintf (header, "system.kernel.options = %s", line);
			fclose (cmdline);
		}
	}
	{
		fflush (header);
		int maxpid = fork();
		if (!maxpid) _exit(0);
		fprintf (header, "system.maxpid = %d\n", maxpid);
	}

	if (header != stdout)
		fclose (header);
	return 0;
}
