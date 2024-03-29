/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Josef Gajdusek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */

#include <argp.h>
#include <limits.h>
#include <math.h>
#include <libgen.h>
#include <proj_api.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bsd/string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <CL/cl.h>
#include <libpng16/png.h>
#include <json-c/json.h>

#include "blank.h"
#include "colormaps.h"
#include "coords.h"
#include "utils.h"
#include "log.h"

#define MAX_SOURCE_SIZE 100000
#define OCLCHECK(x) if ((x) != CL_SUCCESS) { log_error_clerr("OCL Error!", x); exit(EXIT_FAILURE); }
#define TILE_SIZE 256

void write_png(char *fname, int width, int height, uint8_t *img, rgba_t *colormap)
{
	FILE *fout = fopen(fname, "w");
	if (fout == NULL) {
		perror("Failed to write a file\n");
		return;
	}

	// TODO: Error handling!!!
	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop png_info = png_create_info_struct(png_ptr);

	if (setjmp(png_jmpbuf(png_ptr))) {
		log_error("Error during png creation");
		return;
	}

	png_init_io(png_ptr, fout);

	png_colorp colors = calloc(COLORMAP_LEN, sizeof(png_color));
	png_bytep trns = calloc(COLORMAP_LEN, sizeof(png_color));
	for (unsigned i = 0; i < COLORMAP_LEN; i++) {
		colors[i].red = colormap[i].r;
		colors[i].green = colormap[i].g;
		colors[i].blue = colormap[i].b;
		trns[i] = colormap[i].a;
	}

	png_set_PLTE(png_ptr, png_info, colors, COLORMAP_LEN);
	png_set_tRNS(png_ptr, png_info, trns, COLORMAP_LEN, NULL);
	png_set_IHDR(png_ptr, png_info, width, height, 8, PNG_COLOR_TYPE_PALETTE,
				 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

	png_write_info(png_ptr, png_info);

	free(colors);
	free(trns);

	// TODO: We actually want pallete colors
	png_bytep rowp = malloc(1 * width * sizeof(png_byte));
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			rowp[x] = img[y * height + x];
		}
		png_write_row(png_ptr, rowp);
	}
	free(rowp);

	png_write_end(png_ptr, NULL);

	fclose(fout);
	png_free_data(png_ptr, png_info, PNG_FREE_ALL, -1);
	free(png_info);
	png_destroy_write_struct(&png_ptr, NULL);
}

struct arguments {
	int zoomlevel;
	unsigned int platformid;
	unsigned int deviceid;
	char *kernel;
	char *jspath;
	char *outdir;
	char *clargs;
	struct rect bounds;
	bool bounds_defined;
	rgba_t *colormap;
	projPJ proj_meters;
	float prefilter;
};

const char *argp_program_version = "cl-heatmap 1.0";
const char *argp_program_bug_address = "<atx@atx.name>";
static const char argp_doc[] = "TODO";

static struct argp_option argp_opts[] = {
	{ "zoom",		'z',	"ZOOM",			0,	"Zoomlevel", 0 },
	{ "kernel",	'k',	"KERNEL",		0,	"Kernel to use", 0 },
	{ "outdir",	'o',	"OUTDIR",		0,	"Output directory", 0 },
	{ "input",	'i',	"INPUT",		0,	"Input JSON", 0 },
	{ "clargs",	'c',	"CLARGS",		0,	"OpenCL compiler arguments", 0 },
	{ "colormap",	'm',	"COLORMAP",		0,	"Colormap to use, available: [\"heat\"]", 0 },
	{ "boundaries",'b',	"BOUNDARIES",	0,	"Boundaries in WGS84 '50.12,14.23,51.23,15.33'", 0 },
	{ "device",	'd',	"DEVICE",		0,	"OpenCL device to use (-d 0.0)", 0 },
	{ "projection",'p',	"PROJECTION",	0,	"Proj4 specification of the cartesian projection (default=\"+init=epsg:3045\")", 0 },
	{ "prefilter", 'f', "PREFILTER",	0,	"Do not pass a point to the kernel if it is further than PREFILTER", 0 },
	{ NULL,		0,		NULL,			0,	NULL, 0 }
};

static void parse_opt_boundaries(char *arg, struct argp_state *state)
{
	struct arguments *arguments = state->input;
	float flts[4];
	char *save;
	char *end;

	for (size_t i = 0; i < ARRAY_SIZE(flts); i++, arg = NULL) {
		char *tok = strtok_r(arg, ",", &save);
		if (tok == NULL) {
			argp_error(state, "Error while parsing boundary specification!");
			return;
		}
		flts[i] = strtof(tok, &end);
		if (*end != '\0') {
			argp_error(state, "Error while parsing boundary specification!");
			return;
		}
	}

	arguments->bounds = rect_make((cl_float2){ .x = flts[0], .y = flts[1] },
								  (cl_float2){ .x = flts[2], .y = flts[3] });
	arguments->bounds_defined = true;
}

static long safe_parse_long(struct argp_state *state, char *name, char *arg)
{
	char *end = NULL;
	long ret = strtol(arg, &end, 10);
	if (*end != '\0') {
		argp_error(state, "%s has to be an integer!", name);
		return 0; // We shouldn't get here
	}
	return ret;
}

static double safe_parse_double(struct argp_state *state, char *name, char *arg)
{
	char *end = NULL;
	float ret = strtod(arg, &end);
	if (*end != '\0') {
		argp_error(state, "%s has to be an integer!", name);
		return 0; // We shouldn't get here
	}
	return ret;
}

static void parse_device(char *arg, struct argp_state *state)
{
	struct arguments *arguments = state->input;
	unsigned int *ids[] = { &arguments->platformid, &arguments->deviceid };
	char *save;

	for (size_t i = 0; i < ARRAY_SIZE(ids); i++, arg = NULL) {
		char *tok = strtok_r(arg, ".", &save);
		if (tok == NULL) {
			argp_error(state, "Error while parsing device specification!");
		}
		*ids[i] = safe_parse_long(state, i == 0 ? "PLATFORMID" : "DEVICEID", tok);
	}
}

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct arguments *arguments = state->input;
	switch (key) {
		case 'z':
			arguments->zoomlevel = safe_parse_long(state, "ZOOM", arg);
			break;
		case 'k':
			arguments->kernel = arg;
			break;
		case 'o':
			arguments->outdir = arg;
			break;
		case 'i':
			arguments->jspath = arg;
			break;
		case 'c':
			arguments->clargs = arg;
			break;
		case 'm':
			if (!strcmp(arg, "heat")) {
				arguments->colormap = colormap_heat;
			} else if (!strcmp(arg, "grayscale")) {
				arguments->colormap = colormap_grayscale;
			} else {
				argp_error(state, "Unknown colormap specified!");
			}
			break;
		case 'b':
			parse_opt_boundaries(arg, state);
			break;
		case 'd':
			parse_device(arg, state);
			break;
		case 'p':
			arguments->proj_meters = pj_init_plus(arg);
			if (arguments->proj_meters == NULL) {
				argp_error(state, "Failed to initialize projection: %s", pj_strerrno(pj_errno));
			}
			break;
		case 'f':
			arguments->prefilter = safe_parse_double(state, "PREFILTER", arg);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { argp_opts, parse_opt, NULL, argp_doc, NULL, NULL, NULL };

static void fetch_tile_transform(int z, int x, int y, char *cachedir,
								 projPJ proj_meters, cl_float4 *out)
{
	char dirpath[PATH_MAX];
	snprintf(dirpath, sizeof(dirpath),
			"%s/%d/%d", cachedir, z, x);
	char path[PATH_MAX];
	snprintf(path, sizeof(path),
			"%s/%d.map", dirpath, y);
	FILE *file = fopen(path, "r");
	// TODO: Maybe add more error checks?
	if (file == NULL) {
		generate_translation_tile(x, y, z, out, proj_meters);
		int ret = mkdir_recursive(dirpath, S_IRWXU);
		if (ret < 0) {
			log_error_errno("Failed to mkdir %s", dirpath);
		}
		FILE *fout = fopen(path, "w");
		if (fout == NULL) {
			log_error_errno("Failed to save tile transform cache file %s", path);
			return;
		}
		log_info("Generated cache file %s", path);
		fwrite(out, sizeof(out[0]), 2, fout);
		fclose(fout);
	} else {
		log_info("Loaded cache file %s", path);
		fread(out, sizeof(out[0]), 2, file);
		fclose(file);
	}
}

static char *load_kernel(const char *name, char **retpath)
{
	char *data = NULL;
	size_t len = 0;
	if (strchr(name, '/')) {
		// Consider it an absolute name
		int ret = file_read_whole(name, &data, &len);
		if (ret < 0) {
			perror("Failed to load kernel");
			return NULL;
		}
		*retpath = strdup(name);
	} else {
		const char *paths[] = {
			"./",
			"../kernels", // For running from build/
			"./kernels",
			"/usr/share/cl-heatmap/kernels",
			"/usr/local/share/cl-heatmap/kernels",
		};

		for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s", paths[i], name);

			// Okay, technically we can have OpenCL sources without .cl
			if (!strends(path, ".cl")) {
				strlcat(path, ".cl", sizeof(path));
			}

			int ret = file_read_whole(path, &data, &len);
			if (ret >= 0) {
				*retpath = strdup(path);
				break;
			}
		}
	}

	if (data != NULL) {
		// Zero-terminate the sources
		data = realloc(data, len + 1);
		data[len] = '\0';
	}

	return data;
}


int main(int argc, char *argv[])
{
	struct arguments args = {
		.zoomlevel = 12,
		.platformid = 0,
		.deviceid = 0,
		.kernel = NULL,
		.jspath = "./input.json",
		.outdir = "./cache",
		.clargs = "",
		.bounds_defined = false,
		.colormap = colormap_heat,
		.proj_meters = NULL,
		.prefilter = INFINITY
	};

	argp_parse(&argp, argc, argv, 0, 0, &args);

	if (args.kernel == NULL) {
		fprintf(stderr, "No kernel specified. Select on from the kernels/ directory!\n");
		return EXIT_FAILURE;
	}

	if (!args.bounds_defined) {
		fprintf(stderr, "No boundaries specified!\n");
		return EXIT_FAILURE;
	}

	init_projs();

	// Parse the input JSON
	char *jsonstr;
	if (file_read_whole(args.jspath, &jsonstr, NULL)) {
		log_error_errno("Failed to read the input JSON file %s", args.jspath);
		return EXIT_FAILURE;
	}
	struct json_object *jroot = json_tokener_parse(jsonstr);
	struct json_object *jpts;
	if (!json_object_object_get_ex(jroot, "points", &jpts)) {
		log_error("Key \"points\" not found in the input file");
		return EXIT_FAILURE;
	}
	size_t datalen = json_object_array_length(jpts);
	cl_float2 *datapts = calloc(datalen, sizeof(cl_float2));
	float *datavals = calloc(datalen, sizeof(float));
	for (unsigned int i = 0; i < datalen; i++) {
		json_object *jpt = json_object_array_get_idx(jpts, i);
		json_object *jloc;
		json_object_object_get_ex(jpt, "loc", &jloc);
		json_object *jval;
		json_object_object_get_ex(jpt, "val", &jval);
		json_object *jlat = json_object_array_get_idx(jloc, 0);
		json_object *jlng = json_object_array_get_idx(jloc, 1);
		datapts[i].x = json_object_get_double(jlat);
		datapts[i].y = json_object_get_double(jlng);
		datavals[i] = json_object_get_double(jval);
	}

	log_info("Loaded %ld points", datalen);

	if (args.proj_meters == NULL) {
		args.proj_meters = pj_init_plus("+init=epsg:3045");
	}

	for (unsigned int i = 0; i < datalen; i++) {
		datapts[i] = wgs84_to_meters(datapts[i], args.proj_meters);
	}

	struct rect tilebounds = rect_make(
			wgs84_to_tile(args.bounds.lt, args.zoomlevel),
			wgs84_to_tile(args.bounds.rb, args.zoomlevel));
	tilebounds.lt = round_point(tilebounds.lt, 1, false);
	tilebounds.rb = round_point(tilebounds.rb, 1, true);

	// Render tiles
	log_info("Rendering tiles from (%d,%d) to (%d,%d) on zoomlevel %d",
			 (int)rect_left(tilebounds), (int)rect_top(tilebounds),
			 (int)rect_right(tilebounds), (int)rect_bot(tilebounds),
			 args.zoomlevel);


	char zpath[PATH_MAX];
	snprintf(zpath, sizeof(zpath), "%s/%d", args.outdir, args.zoomlevel);
	mkdir(zpath, 0755);

	log_warn("Starting OpenCL!");
	log_debug("Attempting to use platform = %d and device = %d", args.platformid, args.deviceid);

	// Initialize OpenCL
	cl_platform_id pids[10];
	cl_uint cnt;
	clGetPlatformIDs(ARRAY_SIZE(pids), pids, &cnt);
	if (args.platformid > cnt - 1) {
		log_error("Platform id = %d not found!", args.platformid);
		exit(EXIT_FAILURE);
	}
	cl_platform_id platform = pids[args.platformid];
	char platname[500];
	clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(platname), platname, NULL);
	char platver[500];
	clGetPlatformInfo(platform, CL_PLATFORM_VERSION, sizeof(platver), platver, NULL);
	log_info("OpenCL Platform %s  %s", platname, platver);

	cl_device_id dids[10];
	clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, ARRAY_SIZE(dids), dids, &cnt);
	if (args.deviceid > cnt - 1) {
		log_error("Device id = %d not found!", args.deviceid);
		exit(EXIT_FAILURE);
	}
	cl_device_id *devid = &dids[args.deviceid];
	char devname[500];
	clGetDeviceInfo(*devid, CL_DEVICE_NAME, sizeof(devname), devname, NULL);
	char devver[500];
	clGetDeviceInfo(*devid, CL_DEVICE_VERSION, sizeof(devver), devver, NULL);
	log_info("OpenCL Device %s  %s", devname, devver);

	cl_int ret;
	cl_context clctx = clCreateContext(NULL, 1, devid, NULL, NULL, &ret);
	OCLCHECK(ret);
	cl_command_queue clque = clCreateCommandQueue(clctx, *devid, 0, &ret);
	OCLCHECK(ret);

	// Build the kernel
	char *kpath = NULL;
	char *clsrc = load_kernel(args.kernel, &kpath);

	if (!clsrc) {
		return EXIT_FAILURE;
	}

	log_info("Loaded kernel from %s", kpath);

	char *kdir = dirname(kpath);
	char compargs[1000];
	bzero(compargs, sizeof(compargs));
	snprintf(compargs, ARRAY_SIZE(compargs),
			"-I%s -DCOLORS_LEN=%d -DTILE_SIZE=%d %s",
			kdir, COLORMAP_LEN, TILE_SIZE, args.clargs);

	cl_program clprg = clCreateProgramWithSource(clctx, 1, (const char **)&clsrc, NULL, &ret);
	OCLCHECK(ret);
	ret = clBuildProgram(clprg, 1, devid, compargs, NULL, NULL);
	if (ret != CL_SUCCESS) {
		log_error_clerr("Kernel build failed, dumping compiler output", ret);
		size_t len;
		clGetProgramBuildInfo(clprg, *devid, CL_PROGRAM_BUILD_LOG, 0, NULL, &len);
		char msg[len];
		clGetProgramBuildInfo(clprg, *devid, CL_PROGRAM_BUILD_LOG, len, msg, NULL);
		fprintf(stderr, "%s\n", msg);
		return EXIT_FAILURE;
	}
	cl_kernel clkrn = clCreateKernel(clprg, "generate_pixel", &ret);
	OCLCHECK(ret);

	const cl_image_format imformat = { CL_R, CL_UNSIGNED_INT8 };
	const cl_image_desc imdesc = {
		.image_type = CL_MEM_OBJECT_IMAGE2D,
		.image_width = TILE_SIZE,
		.image_height = TILE_SIZE,
		.image_depth = 0,
		.image_array_size = 1,
		.image_row_pitch = 0,
		.image_slice_pitch = 0,
		.num_samples = 0,
		.buffer = NULL
	};
	size_t tile_len = TILE_SIZE * TILE_SIZE * sizeof(uint8_t);
	uint8_t *tile = malloc(tile_len);
	cl_mem tile_cl = clCreateImage(clctx, CL_MEM_WRITE_ONLY, &imformat, &imdesc, NULL, &ret);
	OCLCHECK(ret);
	// Note that we allocate the upper-bound of input points, this should not be
	// a lot of memory anyway
	cl_float2 *chosenpts = calloc(datalen, sizeof(cl_float2));
	cl_mem pts_cl = clCreateBuffer(clctx, CL_MEM_READ_ONLY, datalen * sizeof(cl_float2),
								   NULL, &ret);
	OCLCHECK(ret);
	float *chosenvals = calloc(datalen, sizeof(float));
	cl_mem vals_cl = clCreateBuffer(clctx, CL_MEM_READ_ONLY, datalen * sizeof(cl_float),
									NULL, &ret);
	OCLCHECK(ret);

	char blankfilepath[PATH_MAX];
	snprintf(blankfilepath, sizeof(blankfilepath), "%s/blank.png", args.outdir);
	// We are going to be overwriting the file a few times for different zooms, solve that maybe?
	FILE *file = fopen(blankfilepath, "wb");
	if (file == NULL) {
		// Otherwise, just ignore that, the link() calls later are going to fail, but meh
		log_error_errno("Failed to save the blank tile!");
	} else {
		fwrite(blank_tile_png, 1, sizeof(blank_tile_png), file);
		fclose(file);
	}

	for (unsigned int tx = rect_left(tilebounds); tx <= rect_right(tilebounds); tx++) {
		for (unsigned int ty = rect_top(tilebounds); ty <= rect_bot(tilebounds); ty++) {
			log_info("Processing (%d,%d)", tx, ty);
			cl_float4 tr[2];
			fetch_tile_transform(args.zoomlevel, tx, ty, args.outdir,
								 args.proj_meters, tr);

			// Now we attempt to filter out points which are too far away to make
			// any difference for the tile values
			struct rect tilet = rect_make((cl_float2){ .x = tx	  , .y = ty	   },
										  (cl_float2){ .x = tx + 1, .y = ty + 1});
			// Okay, so we can't just transform the left-top and right-bottom corners
			// here and call it a day as the tile->meters coordinate transformation
			// would need to have axis in the same direction.
			// As we don't care about some extra points being included, we
			// just take the maximum boundary.
			cl_float2 ptstile[4] = {
				rect_lefttop(tilet), rect_righttop(tilet),
				rect_rightbot(tilet), rect_leftbot(tilet),
			};
			cl_float2 ptsms[4];
			for (size_t i = 0; i < ARRAY_SIZE(ptsms); i++) {
				ptsms[i] = tile_to_meters(ptstile[i], args.zoomlevel,
										  args.proj_meters);
			}
			struct rect tilems = rect_max(ptsms, ARRAY_SIZE(ptsms));
			tilems = rect_inflate(tilems, args.prefilter);

			cl_uint npts = 0;
			for (size_t i = 0; i < datalen; i++) {
				if (!rect_is_inside(tilems, datapts[i])) {
					continue;
				}
				chosenpts[npts] = datapts[i];
				chosenvals[npts] = datavals[i];
				npts++;
			}

			char path[PATH_MAX];
			snprintf(path, sizeof(path),
					"%s/%d/%d/%d.png", args.outdir, args.zoomlevel, tx, ty);

			bzero(tile, TILE_SIZE * TILE_SIZE);
			if (npts != 0) {
				log_info(" generating from %d...", npts);
				clEnqueueWriteBuffer(clque, pts_cl, CL_TRUE, 0, npts * sizeof(chosenpts[0]),
									 chosenpts, 0, NULL, NULL);
				clEnqueueWriteBuffer(clque, vals_cl, CL_TRUE, 0, npts * sizeof(chosenvals[0]),
									 chosenvals, 0, NULL, NULL);

				ret = clSetKernelArg(clkrn, 0, sizeof(tr[0]), &tr[0]);
				OCLCHECK(ret);
				ret = clSetKernelArg(clkrn, 1, sizeof(tr[1]), &tr[1]);
				OCLCHECK(ret);
				ret = clSetKernelArg(clkrn, 2, sizeof(npts), &npts);
				OCLCHECK(ret);
				ret = clSetKernelArg(clkrn, 3, sizeof(pts_cl), &pts_cl);
				OCLCHECK(ret);
				ret = clSetKernelArg(clkrn, 4, sizeof(vals_cl), &vals_cl);
				OCLCHECK(ret);
				ret = clSetKernelArg(clkrn, 5, sizeof(tile_cl), &tile_cl);
				OCLCHECK(ret);

				size_t global_work_size[] = { TILE_SIZE, TILE_SIZE };
				size_t local_work_size[] = { 1, 1 };
				ret = clEnqueueNDRangeKernel(clque, clkrn, 2, NULL,
											 global_work_size, local_work_size,
											 0, NULL, NULL);
				OCLCHECK(ret);
				clFinish(clque);

				// Read the image and save to a file
				clEnqueueReadImage(clque, tile_cl, CL_TRUE,
								  (size_t[3]){0, 0, 0},
								  (size_t[3]){TILE_SIZE, TILE_SIZE, 1},
								  0, 0, tile, 0, NULL, NULL);

				write_png(path, TILE_SIZE, TILE_SIZE,
						  tile,
						  args.colormap);
				log_info(" wrote %s", path);
			} else {
				log_info(" skipping...");
				link(blankfilepath, path);
				log_info(" linked %s to %s", path, blankfilepath);
			}
		}
	}

	ret = clFlush(clque);
	ret = clFinish(clque);
	ret = clReleaseKernel(clkrn);
	ret = clReleaseProgram(clprg);
	ret = clReleaseMemObject(vals_cl);
	ret = clReleaseMemObject(pts_cl);
	ret = clReleaseMemObject(tile_cl);
	ret = clReleaseContext(clctx);

	free(chosenvals);
	free(chosenpts);
	free(tile);
	free(clsrc);
}
