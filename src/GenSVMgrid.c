/**
 * @file GenSVMgrid.c
 * @author G.J.J. van den Burg
 * @date 2014-01-07
 * @brief Command line interface for the grid search program
 *
 * @details
 * This is a command line interface to the parameter grid search functionality
 * of the algorithm. The grid search is specified in a separate file, thereby
 * reducing the number of command line arguments. See
 * read_grid_from_file() for documentation on the grid file.
 *
 * The program runs a grid search as specified in the grid file. If
 * desired the grid search can incorporate consistency checks to find the
 * configuration among the best configurations which scores consistently high.
 * All output is written to stdout, unless the quiet mode is specified.
 *
 * For further usage information, see the program help function.
 *
 * @copyright
 Copyright 2016, G.J.J. van den Burg.

 This file is part of GenSVM.

 GenSVM is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 GenSVM is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with GenSVM. If not, see <http://www.gnu.org/licenses/>.

 */

#include "gensvm_checks.h"
#include "gensvm_cmdarg.h"
#include "gensvm_consistency.h"
#include "gensvm_io.h"
#include "gensvm_gridsearch.h"
#include "gensvm_train.h"

/**
 * Minimal number of command line arguments
 */
#define MINARGS 2

extern FILE *GENSVM_OUTPUT_FILE;
extern FILE *GENSVM_ERROR_FILE;

// function declarations
void exit_with_help(char **argv);
long parse_command_line(int argc, char **argv, char *input_filename,
		char **prediction_outputfile);
void read_grid_from_file(char *input_filename, struct GenGrid *grid);

/**
 * @brief Help function
 *
 * @details
 * Print help for this program and exit. Note that VERSION is provided by the
 * Makefile.
 *
 * @param[in] 	argv 	command line arguments
 *
 */
void exit_with_help(char **argv)
{
	printf("This is GenSVM, version %s.\n", VERSION_STRING);
	printf("Copyright (C) 2016, G.J.J. van den Burg.\n");
	printf("This program is free software, see the LICENSE file "
			"for details.\n\n");
	printf("Usage: %s [options] grid_file\n", argv[0]);
	printf("Options:\n");
	printf("-h | -help : print this help.\n");
	printf("-o prediction_output : write predictions of test data to "
			"file (uses stdout if not provided)\n");
	printf("-q         : quiet mode (no output, not even errors!)\n");
	printf("-x         : data files are in LibSVM/SVMlight format\n");
	printf("-z         : seed for the random number generator\n");

	exit(EXIT_FAILURE);
}

/**
 * @brief Main interface function for GenSVMgrid
 *
 * @details
 * Main interface for the command line program. A given grid file which
 * specifies a grid search over a single dataset is read. From this, a Queue
 * is created containing all Task instances that need to be performed in the
 * search. Depending on the type of dataset, either cross validation or
 * train/test split training is performed for all tasks. If specified,
 * consistency repeats are done at the end of the grid search. Note that
 * currently no output is produced other than what is written to stdout.
 *
 * @param[in] 	argc 	number of command line arguments
 * @param[in] 	argv 	array of command line arguments
 *
 * @return 		exit status
 *
 */
int main(int argc, char **argv)
{
	int i, best_ID = -1;
	long seed;
	bool libsvm_format = false;
	char input_filename[GENSVM_MAX_LINE_LENGTH];
	char *prediction_outputfile = NULL;

	struct GenGrid *grid = gensvm_init_grid();
	struct GenData *train_data = gensvm_init_data();
	struct GenData *test_data = gensvm_init_data();
	struct GenQueue *q = gensvm_init_queue();

	if (argc < MINARGS || gensvm_check_argv(argc, argv, "-help")
			|| gensvm_check_argv_eq(argc, argv, "-h") )
		exit_with_help(argv);
	seed = parse_command_line(argc, argv, input_filename,
			&prediction_outputfile);
	libsvm_format = gensvm_check_argv(argc, argv, "-x");

	note("Reading grid file\n");
	read_grid_from_file(input_filename, grid);

	note("Reading data from %s\n", grid->train_data_file);
	if (libsvm_format)
		gensvm_read_data_libsvm(train_data, grid->train_data_file);
	else
		gensvm_read_data(train_data, grid->train_data_file);

	// Read the test data if present
	if (grid->test_data_file != NULL) {
		if (libsvm_format)
			gensvm_read_data_libsvm(test_data,
					grid->test_data_file);
		else
			gensvm_read_data(test_data, grid->test_data_file);
	} else {
		gensvm_free_data(test_data);
		test_data = NULL;
	}

	// check labels of training data
	gensvm_check_outcome_contiguous(train_data);
	if (!gensvm_check_outcome_contiguous(train_data)) {
		err("[GenSVM Error]: Class labels should start from 1 and "
				"have no gaps. Please reformat your data.\n");
		exit(EXIT_FAILURE);
	}

	// check if we are sparse and want nonlinearity
	if (train_data->Z == NULL && grid->kerneltype != K_LINEAR) {
		err("[GenSVM Warning]: Sparse matrices with nonlinear kernels "
				"are not yet supported. Dense matrices will "
				"be used.\n");
		train_data->RAW = gensvm_sparse_to_dense(train_data->spZ);
		train_data->Z = train_data->RAW;
		gensvm_free_sparse(train_data->spZ);
	}

	note("Creating queue\n");
	gensvm_fill_queue(grid, q, train_data, test_data);

	srand(seed);

	note("Starting training\n");
	gensvm_train_queue(q);
	note("Training finished\n");

	if (grid->repeats > 0) {
		best_ID = gensvm_consistency_repeats(q, grid->repeats,
				grid->percentile);
	} else {
		double maxperf = -1;
		for (i=0; i<q->N; i++) {
			if (q->tasks[i]->performance > maxperf) {
				maxperf = q->tasks[i]->performance;
				best_ID = q->tasks[i]->ID;
			}
		}
	}

	// If we have test data, train best model on training data and predict
	// test data
	if (test_data) {
		struct GenTask *best_task = NULL;
		struct GenModel *best_model = NULL;
		long *predy = NULL;
		double performance = -1;

		for (i=0; i<q->N; i++)
			if (q->tasks[i]->ID == best_ID)
				best_task = q->tasks[i];

		best_model = gensvm_init_model();
		gensvm_task_to_model(best_task, best_model);

		gensvm_train(best_model, train_data, NULL);

		// check if we are sparse and want nonlinearity
		if (test_data->Z == NULL &&
				best_model->kerneltype != K_LINEAR) {
			err("[GenSVM Warning]: Sparse matrices with nonlinear "
					"kernels are not yet supported. Dense "
					"matrices will be used.\n");
			test_data->Z = gensvm_sparse_to_dense(test_data->spZ);
			gensvm_free_sparse(test_data->spZ);
		}

		gensvm_kernel_postprocess(best_model, train_data, test_data);

		// predict labels
		predy = Calloc(long, test_data->n);
		gensvm_predict_labels(test_data, best_model, predy);

		if (test_data->y != NULL) {
			performance = gensvm_prediction_perf(test_data, predy);
			note("Predictive performance: %3.2f%%\n", performance);
		}

		// if output file is specified, write predictions to it
		if (gensvm_check_argv_eq(argc, argv, "-o")) {
			gensvm_write_predictions(test_data, predy,
				       	prediction_outputfile);
			note("Prediction written to: %s\n",
				       	prediction_outputfile);
		} else {
			for (i=0; i<test_data->n; i++)
				printf("%li ", predy[i]);
			printf("\n");
		}

		gensvm_free_model(best_model);
		free(predy);
	}

	gensvm_free_queue(q);
	gensvm_free_grid(grid);
	gensvm_free_data(train_data);
	gensvm_free_data(test_data);

	note("Done.\n");
	return 0;
}

/**
 * @brief Parse command line arguments
 *
 * @details
 * Few arguments can be supplied to the command line. Only quiet mode can be
 * specified, or help can be requested. The filename of the grid file is
 * read from the arguments. Parsing of the grid file is done separately in
 * read_grid_from_file().
 *
 * @param[in] 	argc 		number of command line arguments
 * @param[in] 	argv 		array of command line arguments
 * @param[in] 	input_filename 	pre-allocated buffer for the grid
 * 				filename.
 * @returns 			seed for the RNG
 *
 */
long parse_command_line(int argc, char **argv, char *input_filename,
		char **prediction_outputfile)
{
	long seed = time(NULL);
	int i;

	GENSVM_OUTPUT_FILE = stdout;
	GENSVM_ERROR_FILE = stderr;

	for (i=1; i<argc; i++) {
		if (argv[i][0] != '-') break;
		if (++i>=argc)
			exit_with_help(argv);
		switch (argv[i-1][1]) {
			case 'o':
				(*prediction_outputfile) = Malloc(char,
						strlen(argv[i]) + 1);
				strcpy((*prediction_outputfile), argv[i]);
				break;
			case 'q':
				GENSVM_OUTPUT_FILE = NULL;
				GENSVM_ERROR_FILE = NULL;
				i--;
				break;
			case 'x':
				i--;
				break;
			case 'z':
				seed = atoi(argv[i]);
				break;
			default:
				fprintf(stderr, "Unknown option: -%c\n",
						argv[i-1][1]);
				exit_with_help(argv);
		}
	}

	if (i >= argc)
		exit_with_help(argv);

	strcpy(input_filename, argv[i]);

	return seed;
}

/**
 * @brief Parse the kernel string from the training file
 *
 * @details
 * This is a utility function for the read_grid_from_file() function, to keep
 * the main code a bit shorter. It reads the line from the given buffer and
 * returns the corresponding KernelType.
 *
 * @param[in] 	kernel_line 	line from the file with the kernel
 * 				specification
 * @return 	the corresponding kerneltype
 */
KernelType parse_kernel_str(char *kernel_line)
{
	if (str_endswith(kernel_line, "LINEAR\n")) {
		return K_LINEAR;
	} else if (str_endswith(kernel_line, "POLY\n")) {
		return K_POLY;
	} else if (str_endswith(kernel_line, "RBF\n")) {
		return K_RBF;
	} else if (str_endswith(kernel_line, "SIGMOID\n")) {
		return K_SIGMOID;
	} else {
		fprintf(stderr, "Unknown kernel specified on line: %s\n",
				kernel_line);
		exit(EXIT_FAILURE);
	}
}

/**
 * @brief Read the GenGrid struct from file
 *
 * @details
 * Read the GenGrid struct from a file. The grid file follows a specific
 * format specified in @ref spec_grid_file.
 *
 * Commonly used string functions in this function are all_doubles_str() and
 * all_longs_str().
 *
 * @param[in] 	input_filename 	filename of the grid file
 * @param[in] 	grid 	GenGrid structure to place the parsed
 * 				parameter grid.
 *
 */
void read_grid_from_file(char *input_filename, struct GenGrid *grid)
{
	long i, nr = 0;
	FILE *fid;
	char buffer[GENSVM_MAX_LINE_LENGTH];
	char train_filename[GENSVM_MAX_LINE_LENGTH];
	char test_filename[GENSVM_MAX_LINE_LENGTH];
	double *params = Calloc(double, GENSVM_MAX_LINE_LENGTH);
	long *lparams = Calloc(long, GENSVM_MAX_LINE_LENGTH);

	fid = fopen(input_filename, "r");
	if (fid == NULL) {
		fprintf(stderr, "Error opening grid file %s\n",
				input_filename);
		exit(EXIT_FAILURE);
	}
	grid->traintype = CV;
	while ( fgets(buffer, GENSVM_MAX_LINE_LENGTH, fid) != NULL ) {
		Memset(params, double,  GENSVM_MAX_LINE_LENGTH);
		Memset(lparams, long, GENSVM_MAX_LINE_LENGTH);
		if (str_startswith(buffer, "train:")) {
			sscanf(buffer, "train: %s\n", train_filename);
			grid->train_data_file = Calloc(char,
					GENSVM_MAX_LINE_LENGTH);
			strcpy(grid->train_data_file, train_filename);
		} else if (str_startswith(buffer, "test:")) {
			sscanf(buffer, "test: %s\n", test_filename);
			grid->test_data_file = Calloc(char,
					GENSVM_MAX_LINE_LENGTH);
			strcpy(grid->test_data_file, test_filename);
		} else if (str_startswith(buffer, "p:")) {
			nr = all_doubles_str(buffer, 2, params);
			grid->ps = Calloc(double, nr);
			for (i=0; i<nr; i++)
				grid->ps[i] = params[i];
			grid->Np = nr;
		} else if (str_startswith(buffer, "lambda:")) {
			nr = all_doubles_str(buffer, 7, params);
			grid->lambdas = Calloc(double, nr);
			for (i=0; i<nr; i++)
				grid->lambdas[i] = params[i];
			grid->Nl = nr;
		} else if (str_startswith(buffer, "kappa:")) {
			nr = all_doubles_str(buffer, 6, params);
			grid->kappas = Calloc(double, nr);
			for (i=0; i<nr; i++)
				grid->kappas[i] = params[i];
			grid->Nk = nr;
		} else if (str_startswith(buffer, "epsilon:")) {
			nr = all_doubles_str(buffer, 8, params);
			grid->epsilons = Calloc(double, nr);
			for (i=0; i<nr; i++)
				grid->epsilons[i] = params[i];
			grid->Ne = nr;
		} else if (str_startswith(buffer, "weight:")) {
			nr = all_longs_str(buffer, 7, lparams);
			grid->weight_idxs = Calloc(int, nr);
			for (i=0; i<nr; i++)
				grid->weight_idxs[i] = lparams[i];
			grid->Nw = nr;
		} else if (str_startswith(buffer, "folds:")) {
			nr = all_longs_str(buffer, 6, lparams);
			grid->folds = lparams[0];
			if (nr > 1)
				fprintf(stderr, "Field \"folds\" only takes "
						"one value. Additional "
						"fields are ignored.\n");
		} else if (str_startswith(buffer, "repeats:")) {
			nr = all_longs_str(buffer, 8, lparams);
			grid->repeats = lparams[0];
			if (nr > 1)
				fprintf(stderr, "Field \"repeats\" only "
						"takes one value. Additional "
						"fields are ignored.\n");
		} else if (str_startswith(buffer, "percentile:")) {
			nr = all_doubles_str(buffer, 11, params);
			grid->percentile = params[0];
			if (nr > 1)
				fprintf(stderr, "Field \"percentile\" only "
						"takes one value. Additional "
						"fields are ignored.\n");
		} else if (str_startswith(buffer, "kernel:")) {
			grid->kerneltype = parse_kernel_str(buffer);
		} else if (str_startswith(buffer, "gamma:")) {
			nr = all_doubles_str(buffer, 6, params);
			if (grid->kerneltype == K_LINEAR) {
				fprintf(stderr, "Field \"gamma\" ignored, "
						"linear kernel is used.\n");
				grid->Ng = 0;
				continue;
			}
			grid->gammas = Calloc(double, nr);
			for (i=0; i<nr; i++)
				grid->gammas[i] = params[i];
			grid->Ng = nr;
		} else if (str_startswith(buffer, "coef:")) {
			nr = all_doubles_str(buffer, 5, params);
			if (grid->kerneltype == K_LINEAR ||
				grid->kerneltype == K_RBF) {
				fprintf(stderr, "Field \"coef\" ignored with "
						"specified kernel.\n");
				grid->Nc = 0;
				continue;
			}
			grid->coefs = Calloc(double, nr);
			for (i=0; i<nr; i++)
				grid->coefs[i] = params[i];
			grid->Nc = nr;
		} else if (str_startswith(buffer, "degree:")) {
			nr = all_doubles_str(buffer, 7, params);
			if (grid->kerneltype != K_POLY) {
				fprintf(stderr, "Field \"degree\" ignored "
						"with specified kernel.\n");
				grid->Nd = 0;
				continue;
			}
			grid->degrees = Calloc(double, nr);
			for (i=0; i<nr; i++)
				grid->degrees[i] = params[i];
			grid->Nd = nr;
		} else {
			fprintf(stderr, "Cannot find any parameters on line: "
					"%s\n", buffer);
		}
	}

	free(params);
	free(lparams);
	fclose(fid);
}
