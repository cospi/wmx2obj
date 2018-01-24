/*
 * wmx2obj - Push Final Fantasy 8 world map geometry to Wavefront OBJ format
 * Copyright (C) (2015-)2018 Aleksanteri Hirvonen
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_FILE_ARG_LOC  1
#define OUT_FILE_ARG_LOC (IN_FILE_ARG_LOC  + 1)
#define NUM_ARGS_MIN     (OUT_FILE_ARG_LOC + 1)
#define START_ARG_LOC    NUM_ARGS_MIN
#define END_ARG_LOC      (START_ARG_LOC + 1)

#define SEGMENT_MIN            0
#define SEGMENT_MAX          834
#define SEGMENTS_PER_ROW      32
#define BLOCKS_PER_SEGMENT    16
#define BLOCKS_PER_ROW         4
#define VERTICES_PER_POLYGON   3

#define SEGMENT_SIZE     0x9000UL
#define SEGMENT_BOUNDS   0x2000UL
#define BLOCK_SIZE       (SEGMENT_SIZE   / BLOCKS_PER_SEGMENT)
#define BLOCK_OFFSET_MAX (SEGMENT_SIZE   - BLOCK_SIZE)
#define BLOCK_BOUNDS     (SEGMENT_BOUNDS / BLOCKS_PER_ROW)

#define GROUP_ID_SIZE        4
#define BLOCK_OFFSET_SIZE    4
#define BLOCK_HEADER_SIZE    4
#define END_OF_BLOCK_PADDING 4

#define POLYGON_SIZE 16UL
#define VERTEX_SIZE   8UL
#define NORMAL_SIZE   8UL

typedef struct VertexIndexData {
	unsigned long int vert_max, prev_vert_max;
} VertexIndexData;

static void
die(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

static unsigned int
parse_uint(const char   *str,
           unsigned int  min,
           unsigned int  max)
{
	char *end_ptr;
	unsigned int ret;

	errno = 0;
	ret = (unsigned int)strtoul(str, &end_ptr, 10);

	if (errno)
		return 0;
	if (end_ptr == str) {
		errno = EINVAL;
		return 0;
	}
	if (ret < min || ret > max) {
		errno = ERANGE;
		return 0;
	}

	return ret;
}

static unsigned int
parse_segment_index(const char   *str,
                    unsigned int  min,
                    unsigned int  max,
                    const char   *fail_msg)
{
	unsigned int ret = parse_uint(str, min, max);
	if (errno)
		die("%s: %s", fail_msg, strerror(errno));
	return ret;
}

static void
parse_segment_range(unsigned int  *start,
                    unsigned int  *end,
                    int            argc,
                    char         **argv)
{
#define GET_SEGMENT(loc, min, max, fail, default_val) \
	(argc > (loc) ? parse_segment_index(argv[(loc)], (min), (max), (fail)) \
	              : (default_val))
	*start = GET_SEGMENT(START_ARG_LOC,
	                     SEGMENT_MIN, SEGMENT_MAX,
	                     "Bad start segment",
	                     SEGMENT_MIN);
	*end   = GET_SEGMENT(END_ARG_LOC,
	                     *start, SEGMENT_MAX,
	                     "Bad end segment",
	                     SEGMENT_MAX);
#undef GET_SEGMENT
}

static unsigned int
limit_within_bounds(unsigned int val)
{
	return val <= BLOCK_BOUNDS ? val : (~val + 1) & 0xFFFF;
}

static int
convert_vertex(unsigned long int    x,
               unsigned long int    z,
               FILE                *out,
               const unsigned char *buf)
{
	static const long double scale = 0.001L;
	unsigned int bx, by, bz;

	bx = limit_within_bounds((unsigned int)(buf[0] | (buf[1] << 8)));
	by = limit_within_bounds((unsigned int)(buf[2] | (buf[3] << 8)));
	bz = limit_within_bounds((unsigned int)(buf[4] | (buf[5] << 8)));

	fprintf(out, "v %.3Lf %.3Lf %.3Lf\n",
	        (x + bx) * scale,
	             by  * scale,
	        (z + bz) * scale);

	return !ferror(out);
}

static int
convert_polygon(VertexIndexData     *vert_idx_data,
                FILE                *out,
                const unsigned char *buf)
{
	int i;

	fputc('f', out);
	for (i = 0; i != VERTICES_PER_POLYGON; ++i) {
		unsigned long int vert =
			  vert_idx_data->prev_vert_max + buf[i];
		fprintf(out, " %lu", vert);
		if (vert > vert_idx_data->vert_max)
			vert_idx_data->vert_max = vert;
	}
	fputc('\n', out);

	return !ferror(out);
}

static int
convert_block(unsigned int         pos,
              unsigned long int    x,
              unsigned long int    z,
              VertexIndexData     *vert_idx_data,
              FILE                *out,
              const unsigned char *buf)
{
	unsigned long int offset_loc, offset;
	unsigned char num_polys, num_verts, num_norms, i;
	int res;

	/*
	 * The segment header starts with a group ID,
	 * after which come the block offsets.
	 */
	offset_loc = GROUP_ID_SIZE + pos * BLOCK_OFFSET_SIZE;
	buf += offset_loc;

	offset = (unsigned long int)(   buf[0]
	                             | (buf[1] <<  8UL)
	                             | (buf[2] << 16UL)
	                             | (buf[3] << 24UL));
	if (offset > BLOCK_OFFSET_MAX) {
		fputs("Block offset too large\n", stderr);
		return 0;
	}

	/*
	 * Obtained offset is from the very beginning,
	 * so offset_loc is subtracted.
	 */
	buf += offset - offset_loc;

	num_polys = buf[0];
	num_verts = buf[1];
	/* Normals are not supported, but used for bounds checking. */
	num_norms = buf[2];

	if (  offset
	    + BLOCK_HEADER_SIZE
	    + num_polys * POLYGON_SIZE
	    + num_verts * VERTEX_SIZE
	    + num_norms * NORMAL_SIZE
	    + END_OF_BLOCK_PADDING
	    > SEGMENT_SIZE) {
		fputs("Block could cause a buffer overflow\n", stderr);
		return 0;
	}

	buf += BLOCK_HEADER_SIZE;

	x += pos % BLOCKS_PER_ROW * BLOCK_BOUNDS;
	z += pos / BLOCKS_PER_ROW * BLOCK_BOUNDS;

	vert_idx_data->prev_vert_max = vert_idx_data->vert_max;

	res = 1;
	for (i = 0; res && i != num_polys; ++i, buf += POLYGON_SIZE)
		res = convert_polygon(vert_idx_data, out, buf);
	for (i = 0; res && i != num_verts; ++i, buf += VERTEX_SIZE)
		res = convert_vertex(x, z, out, buf);

	++vert_idx_data->vert_max;

	return res;
}

static int
convert_segment(unsigned int     pos,
                VertexIndexData *vert_idx_data,
                FILE            *in,
                FILE            *out,
                unsigned char   *buf)
{
	unsigned long int x, z;
	int res;
	unsigned int i;

	if (fread(buf, SEGMENT_SIZE, 1, in) != 1) {
		fputs("Read failed\n", stderr);
		if (feof(in))
			fputs("EOF was reached\n", stderr);
		return 0;
	}

	x = pos % SEGMENTS_PER_ROW * SEGMENT_BOUNDS;
	z = pos / SEGMENTS_PER_ROW * SEGMENT_BOUNDS;

	res = 1;
	for (i = 0; res && i != BLOCKS_PER_SEGMENT; ++i)
		res = convert_block(i, x, z, vert_idx_data, out, buf);
	return res;
}

static int
convert_to_obj(unsigned int  start,
               unsigned int  end,
               FILE         *in,
               FILE         *out)
{
	unsigned char *buf;
	VertexIndexData vert_idx_data;
	unsigned int z0, z1, i, j;
	int res;

	if (fseek(in, (long int)(start * SEGMENT_SIZE), SEEK_SET)) {
		fputs("Seek failed\n", stderr);
		return 0;
	}

	buf = malloc(SEGMENT_SIZE);
	if (!buf) {
		fputs("Out of memory\n", stderr);
		return 0;
	}

	/* Wavefront OBJ vertex indices start from 1. */
	vert_idx_data.prev_vert_max = vert_idx_data.vert_max = 1;

	/* Force output model origin as close to (0, 0, 0) as possible. */
	z0 = start / SEGMENTS_PER_ROW;
	z1 = end   / SEGMENTS_PER_ROW;
	j  = z0 != z1 ? start % SEGMENTS_PER_ROW : 0;

	res = 1;
	for (i = start; res && i <= end; ++i, ++j)
		res = convert_segment(j, &vert_idx_data, in, out, buf);
	free(buf);
	return res;
}

int
main(int    argc,
     char **argv)
{
	unsigned int start, end;
	int res;
	FILE *in, *out;

	if (argc < NUM_ARGS_MIN)
		die("Bad arguments: %s <in> <out> [<start>] [<end>]", argv[0]);

	parse_segment_range(&start, &end, argc, argv);

	in = fopen(argv[IN_FILE_ARG_LOC], "rb");
	if (!in)
		die("Failed to open input file: %s", strerror(errno));

	out = fopen(argv[OUT_FILE_ARG_LOC], "w");
	if (!out) {
		fclose(in);
		die("Failed to open output file: %s", strerror(errno));
	}

	printf("Starting conversion of segments %u-%u to %s\n",
	       start, end, argv[OUT_FILE_ARG_LOC]);
	res = convert_to_obj(start, end, in, out);
	fclose(in);
	fclose(out);
	if (!res)
		die("Conversion failed");
	puts("Conversion successful");

	exit(EXIT_SUCCESS);
}
