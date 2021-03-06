/*
Copyright (C) 2009- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#include "allpairs_compare.h"

#include "cctools.h"
#include "debug.h"
#include "stringtools.h"
#include "xxmalloc.h"
#include "fast_popen.h"
#include "text_list.h"
#include "macros.h"
#include "full_io.h"
#include "getopt_aux.h"
#include "rmonitor_poll.h"

static const char *progname = "allpairs_multicore";
static const char *extra_arguments = "";
static int block_size = 0;
static int num_cores = 0;
static int is_symmetric = 0;
static int nindex = 0;
static int index_array[2];

enum {
	LONG_OPT_SYMMETRIC=UCHAR_MAX+1,
	LONG_OPT_INDEX,
};

static void show_help(const char *cmd)
{
	fprintf(stdout, "Usage: %s [options] <set A> <set B> <compare program>\n", cmd);
	fprintf(stdout, "where options are:\n");
	fprintf(stdout, " %-30s Block size: number of items to hold in memory at once. (default: 50%% of RAM\n", "-b,--block-size=<items>");
	fprintf(stdout, " %-30s Number of cores to be used. (default: # of cores in machine)\n", "-c,--cores=<cores>");
	fprintf(stdout, " %-30s Extra arguments to pass to the comparison program.\n", "-e,--extra-args=<args>");
	fprintf(stdout, " %-30s Enable debugging for this subsystem.\n", "-d,--debug=<flag>");
	/* --index and --symmetric can not be used concurrently. */
	fprintf(stdout, " %-30s Specify the indexes of a matrix (used by allpairs_master to specify the indexes of the submatrix for each task).\n", "   --index=\"<xstart> <ystart>\"");
	fprintf(stdout, " %-30s Compute half of a symmetric matrix.\n", "   --symmetric");
	fprintf(stdout, " %-30s Show program version.\n", "-v,--version");
	fprintf(stdout, " %-30s Display this message.\n", "-h,--help");
}

static int get_file_size( const char *path )
{
	struct stat info;
	if(stat(path,&info)==0) {
		return info.st_size;
	} else {
		return 0;
	}
}

/*
block_size_estimate computes how many items we can effectively
get in memory at once by measuring the first 100 elements of the set,
and then choosing a number to fit within 1/2 of the available RAM.
*/

int block_size_estimate( struct text_list *seta, struct rmsummary *tr )
{
	int count = MIN(100,text_list_size(seta));
	int i;
	UINT64_T total_data = 0,total_mem;
	int block_size;

	for(i=0;i<count;i++) {
		total_data += get_file_size(text_list_get(seta,i));
	}

	total_mem = tr->memory * MEGABYTE / 2;

	if(total_data>=total_mem) {
		block_size = text_list_size(seta) * total_mem / total_data;
		if(block_size<1) block_size = 1;
		if(block_size>text_list_size(seta)) block_size = text_list_size(seta);
	} else {
		block_size = text_list_size(seta);
	}

	return block_size;
}

/*
Load the named file into memory, returning the actual data of
the file, and filling length with the length of the buffer in bytes.
The result should be free()d when done.
*/

char * load_one_file( const char *filename, int *length )
{
	FILE *file = fopen(filename,"r");
	if(!file) {
		fprintf(stderr,"%s: couldn't open %s: %s\n",progname,filename,strerror(errno));
		exit(1);
	}

	fseek(file,0,SEEK_END);
	*length = ftell(file);
	fseek(file,0,SEEK_SET);

	char *data = malloc(*length);
	if(!data) {
		fprintf(stderr,"%s: out of memory!\n",progname);
		exit(1);
	}

	full_fread(file,data,*length);
	fclose(file);

	return data;
}

/*
pthreads requires that we pass all arguments through as a single
pointer, so we are forced to use a little structure to send all
of the desired arguments.
*/

struct thread_args {
	allpairs_compare_t func;
	char **xname;
	char **xdata;
	int   *xdata_length;
	int   *xdata_id; //the index array of xdata in the matrix constructed by seta and setb
	char  *yname;
	char  *ydata;
	int    ydata_length;
	int ydata_id; //the index of ydata in the matrix constructed by seta and setb
};

/*
A single thread will loop over a whole block-row of the results,
calling the comparison function once for each pair of items.
*/

static void * row_loop_threaded( void *args )
{
	int i;
	struct thread_args *targs = args;
	if(nindex == 2) {
		for(i=0;i<block_size;i++) {
			if((index_array[0] + targs->xdata_id[i]) <= (index_array[1] + targs->ydata_id)) //calcuate xindex and yindex of the unit in the original matrix of allpairs_master
				targs->func(targs->xname[i],targs->xdata[i],targs->xdata_length[i],targs->yname,targs->ydata,targs->ydata_length);
		}
	} else if (is_symmetric) {
		for(i=0;i<block_size;i++) {
			if(targs->xdata_id[i] <= targs->ydata_id)
				targs->func(targs->xname[i],targs->xdata[i],targs->xdata_length[i],targs->yname,targs->ydata,targs->ydata_length);
		}
	} else {
		for(i=0;i<block_size;i++) {
			targs->func(targs->xname[i],targs->xdata[i],targs->xdata_length[i],targs->yname,targs->ydata,targs->ydata_length);
		}
	}
	return 0;
}

/*
The threaded main loop loads an entire block of objects into memory,
then forks threads, one for each row in the block, until done.
This only applies to functions loaded via dynamic linking.
Up to num_cores threads will be running simultaneously.
*/

static int main_loop_threaded( allpairs_compare_t funcptr, struct text_list *seta, struct text_list *setb )
{
	int x,i,j,c;

	char *xname[block_size];
	char *xdata[block_size];
	int xdata_id[block_size];
	int xdata_length[block_size];

	char *yname[num_cores];
	char *ydata[num_cores];
	int ydata_length[num_cores];
	int ydata_id[num_cores];

	struct thread_args args[num_cores];
	pthread_t thread[num_cores];

	/* for each block sized vertical stripe... */
	for(x=0;x<text_list_size(seta);x+=block_size) {

		/* load the horizontal members of the stripe */
		for(i=0;i<block_size;i++) {
			xdata_id[i] = x + i;
			xname[i] = text_list_get(seta,x+i);
			xdata[i] = load_one_file(text_list_get(seta,x+i),&xdata_length[i]);
		}

		/* for each row in the stripe ... */
		for(j=0;j<text_list_size(setb);j+=num_cores) {

			/* don't start more threads than rows remaining. */
			int n = MIN(num_cores,text_list_size(setb)-j);

			/* start one thread working on a whole row. */
			for(c=0;c<n;c++) {
				yname[c] = text_list_get(setb,j+c);
				ydata_id[c] = j + c;
				ydata[c] = load_one_file(text_list_get(setb,j+c),&ydata_length[c]);

				args[c].func         = funcptr;
				args[c].xname        = xname;
				args[c].xdata        = xdata;
				args[c].xdata_length = xdata_length;
				args[c].xdata_id = xdata_id;
				args[c].yname	     = yname[c];
				args[c].ydata        = ydata[c];
				args[c].ydata_length = ydata_length[c];
				args[c].ydata_id = ydata_id[c];
				pthread_create(&thread[c],0,row_loop_threaded,&args[c]);
			}

			/* wait for each one to finish */
			for(c=0;c<n;c++) {
				pthread_join(thread[c],0);
				free(ydata[c]);
			}
		}

		for(i=0;i<block_size;i++) {
			free(xdata[i]);
		}
	}

	return 0;
}

/*
The program-oriented main loop iterates over the result matrix,
forking a comparison function for each result.  Up to num_cores
programes will be running simultaneously.
*/

static int main_loop_program( const char *funcpath, struct text_list *seta, struct text_list *setb )
{
	int x,i,j,c;
	char line[1024];
	FILE *proc[num_cores];

	int xstop = text_list_size(seta);

	/* for each block sized vertical stripe... */
	for(x=0;x<xstop;x+=block_size) {

		/* for each row in the stripe ... */
		for(j=0;j<text_list_size(setb);j++) {

			/* for each group of num_cores in the stripe... */
			for(i=x;i<(x+block_size);i+=num_cores) {

				/* make sure we don't run past the block width */
				int n = MIN(num_cores,x+block_size-i);

				/* make sure we don't run off the width of the array */
				n = MIN(n,xstop-i);

				/* start one process for each core */
				/* if nindex = 2, the real xindex = index_array[0] + i; the real yindex = index_array[1] + j. */
				for(c=0;c<n;c++) {
					if((nindex == 2 && (index_array[0] + i + c) <= (index_array[1] + j)) || //calcuate xindex and yindex of the unit in the original matrix of allpairs_master
						(is_symmetric == 0 && nindex == 0) ||
						(is_symmetric && (i+c) <= j)) {
							sprintf(line,"%s %s %s %s\n",funcpath,extra_arguments,text_list_get(seta,i+c),text_list_get(setb,j));
							proc[c] = fast_popen(line);
							if(!proc[c]) {
								fprintf(stderr,"%s: couldn't execute %s: %s\n",progname,line,strerror(errno));
								return 1;
							}
					}
				}

				/* then finish one process for each core */
				for(c=0;c<n;c++) {
					if((nindex == 2 && (index_array[0] + i + c) <= (index_array[1] + j)) ||
						(is_symmetric == 0 && nindex == 0) ||
						(is_symmetric && (i+c) <= j)) {
							printf("%s\t%s\t",text_list_get(seta,i+c),text_list_get(setb,j));
							int lines = 0;
							while(fgets(line,sizeof(line),proc[c])) {
								printf("%s",line);
								lines++;
							}
							if(lines==0) printf("\n");
							fast_pclose(proc[c]);
					}
				}
			}
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int c;
	int result;

	debug_config(progname);

	static const struct option long_options[] = {
		{"debug", required_argument, 0, 'd'},
		{"help",  no_argument, 0, 'h'},
		{"version", no_argument, 0, 'v'},
		{"block-size", required_argument, 0, 'b'},
		{"cores", required_argument, 0, 'c'},
		{"extra-args", required_argument, 0, 'e'},
		{"symmetric", no_argument, 0, LONG_OPT_SYMMETRIC},
		{"index", required_argument, 0, LONG_OPT_INDEX},
		{0,0,0,0}
	};

	while((c = getopt_long(argc, argv, "b:c:e:d:vh", long_options, NULL)) > -1) {
		switch (c) {
		case 'b':
			block_size = atoi(optarg);
			break;
		case 'c':
			num_cores = atoi(optarg);
			break;
		case 'e':
			extra_arguments = optarg;
			break;
		case 'd':
			debug_flags_set(optarg);
			break;
		case LONG_OPT_INDEX:
			nindex = sscanf(optarg, "%d %d", &index_array[0], &index_array[1]);
			if(nindex != 2) {
				fprintf(stderr, "You must provide two indexes: xstart and ystart.\n");
				show_help(progname);
				exit(0);
			}
			break;
		case LONG_OPT_SYMMETRIC:
			is_symmetric = 1;
			break;
		case 'v':
			cctools_version_print(stdout, progname);
			exit(0);
			break;
		default:
		case 'h':
			show_help(progname);
			exit(0);
			break;
		}
	}

	cctools_version_debug(D_DEBUG, argv[0]);

	if((argc - optind) < 3) {
		show_help(progname);
		exit(1);
	}

	const char * setapath = argv[optind];
	const char * setbpath = argv[optind+1];
	const char * funcpath = argv[optind+2];

	struct text_list *seta = text_list_load(setapath);
	if(!seta) {
		fprintf(stderr, "allpairs_multicore: cannot open %s: %s\n",setapath,strerror(errno));
		exit(1);
	}

	struct text_list *setb = text_list_load(setbpath);
	if(!setb) {
		fprintf(stderr, "allpairs_multicore: cannot open %s: %s\n",setbpath,strerror(errno));
		exit(1);
	}

	struct rmsummary *tr = rmonitor_measure_host(NULL);
	if(num_cores==0) num_cores = tr->cores;
	debug(D_DEBUG,"num_cores: %d\n",num_cores);

	if(block_size==0) block_size = block_size_estimate(seta, tr);
	debug(D_DEBUG,"block_size: %d elements",block_size);
	free(tr);

	allpairs_compare_t funcptr = allpairs_compare_function_get(funcpath);
	if(funcptr) {
		result = main_loop_threaded(funcptr,seta,setb);
	} else {
		if(access(funcpath,X_OK)!=0) {
			fprintf(stderr, "%s: %s is neither an executable program nor an internal function.\n",progname,funcpath);
			return 1;
		}
		result = main_loop_program(funcpath,seta,setb);
	}

	return result;
}

/* vim: set noexpandtab tabstop=4: */
