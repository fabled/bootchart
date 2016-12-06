/*
 * bootchart-output - dumps state from a main bootchart process via unix socket
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Author: Michael Meeks <michael.meeks@novell.com>
 * Copyright (C) 2009-2010 Novell, Inc.
 * Copyright (C) 2016 Timo Teräs <timo.teras@iki.fi>
 */

#include "common.h"

#include <sys/mman.h>
#include <sys/un.h>

/* simple, easy to unwind via ptrace buffer structures */

static Chunk *chunk_alloc (BufferMap *bm, const char *dest)
{
	Chunk *c;

	/* if we run out of buffer, just keep writing to the last buffer */
	if (bm->max_chunk == sizeof (bm->chunks)/sizeof(bm->chunks[0])) {
		static int overflowed = 0;
		if (!overflowed) {
			log ("bootchart-collector - internal buffer overflow! "
				 "did you set hz too high, or is your boot time too long ?\n");
			overflowed = 1;
		}
		c = bm->chunks[bm->max_chunk - 1];
		c->length = 0;
	} else {
		c = calloc (CHUNK_SIZE, 1);
		strncpy (c->dest_stream, dest, sizeof (c->dest_stream));
		c->length = 0;
		bm->chunks[bm->max_chunk++] = c;
	}

	return c;
}

/*
 * Safe to use from a single thread.
 */
BufferFile *
buffer_file_new (BufferMap *bm, const char *output_fname)
{
	BufferFile *b = calloc (sizeof (BufferFile), 1);
	b->bm = bm;
	b->dest = output_fname;
	b->cur = chunk_alloc (b->bm, b->dest);
	return b;
}

void
buffer_file_append (BufferFile *file, const char *str, size_t len)
{
	do {
		unsigned long to_write = MIN (sizeof file->cur->data - file->cur->length, len);
		memcpy (file->cur->data + file->cur->length, str, to_write);
		str += to_write;
		len -= to_write;
		file->cur->length += to_write;
		if (file->cur->length >= sizeof file->cur->data)
			file->cur = chunk_alloc (file->bm, file->dest);
	} while (len > 0);
}

/* dump whole contents of input_fd to the output 'file' */

void
buffer_file_dump (BufferFile *file, int input_fd)
{
	for (;;) {
		ssize_t to_read = sizeof file->cur->data - file->cur->length;

		to_read = read (input_fd, file->cur->data + file->cur->length, to_read);
		if (to_read < 0) {
			perror ("read error");
			break;
		} else if (to_read == 0) {
			break;
		}
		file->cur->length += to_read;
		if (file->cur->length >= sizeof file->cur->data)
			file->cur = chunk_alloc (file->bm, file->dest);
	}
}

void
buffer_file_dump_frame_with_timestamp (BufferFile *file, int input_fd,
				       const char *uptime, size_t uptimelen)
{
	buffer_file_append (file, uptime, uptimelen);

	lseek (input_fd, SEEK_SET, 0);
	buffer_file_dump (file, input_fd);
  
	buffer_file_append (file, "\n", 1);
}

static const struct sockaddr_un collector_address = {
	.sun_family = AF_UNIX,
	.sun_path = "\0""bootchart2-collector"
};

int
collector_listen (void)
{
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		goto err;

	if (bind(fd, (const struct sockaddr *) &collector_address, sizeof collector_address) < 0)
		goto err;

	if (listen(fd, 5) < 0)
		goto err;

	return fd;

err:
	if (fd >= 0)
		close(fd);
	return -1;
}

enum {
	CMD_DUMP_AND_EXIT = 1,
};

static int
read_all (int fd, void *buf, size_t len)
{
	ssize_t n, total = 0;

	while (len) {
		n = read(fd, buf, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return total ? -1 : 0;
		buf += n;
		len -= n;
		total += n;
	}
	return total;
}

static int
write_all (int fd, const void *buf, size_t len)
{
	ssize_t n, total = 0;

	while (len) {
		n = write(fd, buf, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= n;
		total += n;
	}
	return total;
}

static void
collector_send_dump (int fd, BufferMap *bm)
{
	size_t i;

	for (i = 0; i < sizeof bm->chunks / sizeof bm->chunks[0]; i++) {
		write_all(fd, bm->chunks[i], sizeof *bm->chunks[i]);
	}
}

int
collector_handle (int listen_fd, BufferMap *bm, Arguments *args)
{
	DaemonFlags df;
	uint32_t cmd;
	int fd, ret = 0;

	fd = accept(listen_fd, 0, 0);
	if (fd < 0)
		return -1;

	/* This blocks collector - but normally dump terminates the
	 * collection anyway so it is ok */
	if (read_all(fd, &cmd, sizeof cmd) < 0)
		goto err;

	log("Command %08x from client\n", cmd);

	switch (cmd) {
	case CMD_DUMP_AND_EXIT:
		df = (DaemonFlags) {
			.relative_time = args->relative_time,
		};
		write_all(fd, &df, sizeof df);
		collector_send_dump(fd, bm);
		ret = 1;
		break;
	}

err:
	close(fd);
	return ret;
}

int
collector_dump (const char *output_path, DaemonFlags *df)
{
	char fname[PATH_MAX];
	size_t bytes_dumped = 0;
	uint32_t cmd = CMD_DUMP_AND_EXIT;
	Chunk c;
	int fd, r, ret = -1;
	FILE *out;

	fd = socket (AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		goto err;

	if (connect (fd, (const struct sockaddr *) &collector_address, sizeof collector_address) < 0)
		goto err;

	if (write_all (fd, &cmd, sizeof cmd) < 0)
		goto err;

	if (read_all (fd, df, sizeof *df) < 0)
		goto err;

	while ((r = read_all (fd, &c, sizeof c)) > 0) {
		snprintf (fname, sizeof fname - 1, "%s/%s", output_path, c.dest_stream);
		out = fopen (fname, "a+");
		fwrite (c.data, 1, c.length, out);
		bytes_dumped += c.length;
		fclose (out);
	}
	if (r < 0)
		goto err;

	log ("wrote %ld kb\n", (long)(bytes_dumped+1023)/1024);
	ret = 0;

err:
	if (fd >= 0)
		close(fd);
	return ret;
}

