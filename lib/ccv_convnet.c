#include "ccv.h"
#include "ccv_internal.h"
#ifdef HAVE_GSL
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#endif
#ifdef HAVE_CUDA
#include "cuda/cwc.h"
#endif

inline static void _ccv_convnet_layer_deduce_output_format(ccv_convnet_layer_t* layer, int* rows, int* cols)
{
	assert(rows != 0 && cols != 0);
	switch(layer->type)
	{
		case CCV_CONVNET_CONVOLUTIONAL:
			assert(layer->net.convolutional.rows % 2); // as of now, don't support even number of kernel size
			assert(layer->net.convolutional.cols % 2);
			// assert((layer->input.matrix.rows + layer->net.convolutional.border * 2 - layer->net.convolutional.rows) % layer->net.convolutional.strides == 0);
			// assert((layer->input.matrix.cols + layer->net.convolutional.border * 2 - layer->net.convolutional.cols) % layer->net.convolutional.strides == 0);
			*rows = (layer->input.matrix.rows + layer->net.convolutional.border * 2 - layer->net.convolutional.rows + layer->net.convolutional.strides - 1) / layer->net.convolutional.strides + 1;
			*cols = (layer->input.matrix.cols + layer->net.convolutional.border * 2 - layer->net.convolutional.cols + layer->net.convolutional.strides - 1) / layer->net.convolutional.strides + 1;
			break;
		case CCV_CONVNET_FULL_CONNECT:
			*rows = layer->net.full_connect.count;
			*cols = 1;
			break;
		case CCV_CONVNET_LOCAL_RESPONSE_NORM:
			*rows = layer->input.matrix.rows;
			*cols = layer->input.matrix.cols;
			break;
		case CCV_CONVNET_MAX_POOL:
		case CCV_CONVNET_AVERAGE_POOL:
			// assert((layer->input.matrix.rows + layer->net.pool.border * 2 - layer->net.pool.size) % layer->net.pool.strides == 0);
			// assert((layer->input.matrix.cols + layer->net.pool.border * 2 - layer->net.pool.size) % layer->net.pool.strides == 0);
			*rows = (layer->input.matrix.rows + layer->net.pool.border * 2 - layer->net.pool.size + layer->net.pool.strides - 1) / layer->net.pool.strides + 1;
			*cols = (layer->input.matrix.cols + layer->net.pool.border * 2 - layer->net.pool.size + layer->net.pool.strides - 1) / layer->net.pool.strides + 1;
			break;
	}
}

#ifndef CASE_TESTS

ccv_convnet_t* ccv_convnet_new(int use_cwc_accel, ccv_convnet_layer_param_t params[], int count)
{
	ccv_convnet_t* convnet = (ccv_convnet_t*)ccmalloc(sizeof(ccv_convnet_t) + sizeof(ccv_convnet_layer_t) * count + sizeof(ccv_dense_matrix_t*) * count * 2 + sizeof(ccv_dense_matrix_t*) * (count - 1));
	convnet->use_cwc_accel = use_cwc_accel;
	gsl_rng_env_setup();
	gsl_rng* rng = gsl_rng_alloc(gsl_rng_default);
	gsl_rng_set(rng, (unsigned long int)convnet);
	convnet->reserved = 0;
	convnet->layers = (ccv_convnet_layer_t*)(convnet + 1);
	convnet->acts = (ccv_dense_matrix_t**)(convnet->layers + count);
	memset(convnet->acts, 0, sizeof(ccv_dense_matrix_t*) * count);
	convnet->denoms = (ccv_dense_matrix_t**)(convnet->acts + count);
	memset(convnet->denoms, 0, sizeof(ccv_dense_matrix_t*) * count);
	if (count > 1) 
	{
		convnet->dropouts = (ccv_dense_matrix_t**)(convnet->acts + count * 2);
		memset(convnet->dropouts, 0, sizeof(ccv_dense_matrix_t*) * (count - 1));
	} else {
		convnet->dropouts = 0;
	}
	convnet->count = count;
	convnet->rows = params[0].input.matrix.rows;
	convnet->cols = params[0].input.matrix.cols;
	convnet->channels = params[0].input.matrix.channels;
	ccv_convnet_layer_t* layers = convnet->layers;
	int i, j;
	for (i = 0; i < count; i++)
	{
		layers[i].type = params[i].type;
		layers[i].input = params[i].input;
		layers[i].net = params[i].output;
		switch (params[i].type)
		{
			case CCV_CONVNET_CONVOLUTIONAL:
				layers[i].wnum = params[i].output.convolutional.rows * params[i].output.convolutional.cols * params[i].output.convolutional.channels * params[i].output.convolutional.count;
				layers[i].w = (float*)ccmalloc(sizeof(float) * (layers[i].wnum + params[i].output.convolutional.count));
				layers[i].bias = layers[i].w + layers[i].wnum;
				for (j = 0; j < layers[i].wnum; j++)
					layers[i].w[j] = gsl_ran_gaussian(rng, params[i].sigma);
				for (j = 0; j < params[i].output.convolutional.count; j++)
					layers[i].bias[j] = params[i].bias;
				break;
			case CCV_CONVNET_FULL_CONNECT:
				layers[i].wnum = params[i].input.node.count * params[i].output.full_connect.count;
				layers[i].w = (float*)ccmalloc(sizeof(float) * (layers[i].wnum + params[i].output.full_connect.count));
				layers[i].bias = layers[i].w + layers[i].wnum;
				for (j = 0; j < layers[i].wnum; j++)
					layers[i].w[j] = gsl_ran_gaussian(rng, params[i].sigma);
				for (j = 0; j < params[i].output.full_connect.count; j++)
					layers[i].bias[j] = params[i].bias;
				break;
			case CCV_CONVNET_MAX_POOL:
			case CCV_CONVNET_AVERAGE_POOL:
				layers[i].wnum = 0;
				layers[i].w = 0;
				layers[i].bias = 0;
				break;
		}
	}
	gsl_rng_free(rng);
	return convnet;
}

int ccv_convnet_verify(ccv_convnet_t* convnet, int output)
{
	int i, out_rows, out_cols;
	for (i = 0; i < convnet->count; i++)
	{
		ccv_convnet_layer_t* layer = convnet->layers + i;
		if (i > 0 && (out_rows != layer->input.matrix.rows || out_cols != layer->input.matrix.cols))
			return -1;
		_ccv_convnet_layer_deduce_output_format(layer, &out_rows, &out_cols);
	}
	if (out_rows * out_cols != output)
		return -1;
	return 0;
}

#endif

static void _ccv_convnet_convolutional_forward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t* d, ccv_dense_matrix_t** b)
{
	int rows, cols;
	_ccv_convnet_layer_deduce_output_format(layer, &rows, &cols);
	int ch = layer->net.convolutional.channels;
	int count = layer->net.convolutional.count;
	int strides = layer->net.convolutional.strides;
	int border = layer->net.convolutional.border;
	int kernel_rows = layer->net.convolutional.rows;
	int kernel_cols = layer->net.convolutional.cols;
	int type = CCV_32F | count;
	assert(CCV_GET_CHANNEL(a->type) == ch);
	assert(CCV_GET_DATA_TYPE(a->type) == CCV_32F);
	ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, rows, cols, type, type, 0);
	int i, j, x, y, k;
#define for_block(act_block_setup, act_block_begin, act_block_end) \
	for (k = 0; k < count; k++) \
	{ \
		float* ap = a->data.f32; \
		float* bp = db->data.f32 + k; \
		float* layer_w = layer->w + k * kernel_rows * kernel_cols * ch; \
		float bias = layer->bias[k]; \
		act_block_setup; \
		for (i = 0; i < db->rows; i++) \
		{ \
			int comy = ccv_max(i * strides - border, 0) - (i * strides - border); \
			int maxy = kernel_rows - comy - (i * strides + kernel_rows - ccv_min(a->rows + border, i * strides + kernel_rows)); \
			comy *= ch * kernel_cols; \
			for (j = 0; j < db->cols; j++) \
			{ \
				act_block_begin; \
				float v = bias; \
				int comx = (ccv_max(j * strides - border, 0) - (j * strides - border)) * ch; \
				int maxx = kernel_cols * ch - comx - (j * strides + kernel_cols - ccv_min(a->cols + border, j * strides + kernel_cols)) * ch; \
				float* w = layer_w + comx + comy; \
				float* apz = ap + ccv_max(j * strides - border, 0) * ch; \
				/* when we have border, we simply do zero padding */ \
				for (y = 0; y < maxy; y++) \
				{ \
					for (x = 0; x < maxx; x++) \
						v += w[x] * apz[x]; \
					w += kernel_cols * ch; \
					apz += a->cols * ch; \
				} \
				bp[j * count] = ccv_max(0, v) /* ReLU */; \
				act_block_end; \
			} \
			bp += db->cols * count; \
			ap += a->cols * ch * (ccv_max((i + 1) * strides - border, 0) - ccv_max(i * strides - border, 0)); \
		} \
	}
	if (d)
	{
#define act_block_setup \
		int* dp = d->data.i32 + k;
#define act_block_begin \
		if (!*dp) \
		{
#define act_block_end \
		} else \
			bp[j * count] = 0; \
		dp += count;
		for_block(act_block_setup, act_block_begin, act_block_end);
#undef act_block_setup
#undef act_block_begin
#undef act_block_end
	} else {
		for_block(/* empty act block setup */, /* empty act block begin */, /* empty act block end */);
	}
#undef for_block
}

static void _ccv_convnet_full_connect_forward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t* d, ccv_dense_matrix_t** b)
{
	assert(CCV_GET_DATA_TYPE(a->type) == CCV_32F);
	ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, layer->net.full_connect.count, 1, CCV_32F | CCV_C1, CCV_32F | CCV_C1, 0);
	int ch = CCV_GET_CHANNEL(a->type);
	int rows = a->rows, cols = a->cols;
	// reshape a for gemm
	assert(a->step == a->cols * CCV_GET_DATA_TYPE_SIZE(a->type) * ch);
	a->rows = rows * cols * ch, a->cols = 1, a->type = (a->type - ch) | CCV_C1;
	assert(a->rows * db->rows == layer->wnum);
	a->step = a->cols * CCV_GET_DATA_TYPE_SIZE(a->type);
	int i;
	float* bptr = db->data.f32;
	if (d)
	{
		int j;
		float* aptr = a->data.f32;
		float* wptr = layer->w;
		int* dptr = d->data.i32;
		for (i = 0; i < db->rows; i++)
		{
			if (!dptr[i])
			{
				float v = layer->bias[i];
				for (j = 0; j < a->rows; j++)
					v += aptr[j] * wptr[j];
				wptr += a->rows;
				bptr[i] = v;
			} else
				bptr[i] = 0;
		}
	} else {
		for (i = 0; i < db->rows; i++)
			bptr[i] = layer->bias[i];
		ccv_dense_matrix_t dw = ccv_dense_matrix(db->rows, a->rows, CCV_32F | CCV_C1, layer->w, 0);
		ccv_gemm(&dw, a, 1, db, 1, 0, (ccv_matrix_t**)&db, 0); // supply db as matrix C is allowed
	}
	a->rows = rows, a->cols = cols, a->type = (a->type - CCV_GET_CHANNEL(a->type)) | ch;
	a->step = a->cols * CCV_GET_DATA_TYPE_SIZE(a->type) * CCV_GET_CHANNEL(a->type);
}

static void _ccv_convnet_rnorm_forward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t** b, ccv_dense_matrix_t** denoms)
{
	int rows, cols;
	_ccv_convnet_layer_deduce_output_format(layer, &rows, &cols);
	int size = layer->net.rnorm.size;
	float kappa = layer->net.rnorm.kappa;
	float alpha = layer->net.rnorm.alpha;
	float beta = layer->net.rnorm.beta;
	int way = size / 2;
	assert(CCV_GET_DATA_TYPE(a->type) == CCV_32F);
	int ch = CCV_GET_CHANNEL(a->type);
	int type = CCV_32F | ch;
	ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, rows, cols, type, type, 0);
	ccv_dense_matrix_t* ddenoms = *denoms = ccv_dense_matrix_renew(*denoms, rows, cols, type, type, 0);
	int i, j, k, x;
	float* ap = a->data.f32;
	float* dp = ddenoms->data.f32;
	float* bp = db->data.f32;
	for (i = 0; i < db->rows; i++)
	{
		for (j = 0; j < db->cols; j++)
			for (k = 0; k < ch; k++)
			{
				float v = ap[j * ch + k];
				float denom = 0;
				for (x = ccv_max(k - way, 0); x <= ccv_min(k + way, ch - 1); x++)
					denom += ap[j * ch + x] * ap[j * ch + x];
				denom = kappa + alpha * denom;
				dp[j * ch + k] = denom;
				bp[j * ch + k] = v * powf(denom, -beta);
			}
		ap += a->cols * ch;
		dp += ddenoms->cols * ch;
		bp += db->cols * ch;
	}
}

static void _ccv_convnet_max_pool_forward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t** b)
{
	int rows, cols;
	_ccv_convnet_layer_deduce_output_format(layer, &rows, &cols);
	int size = layer->net.pool.size;
	int strides = layer->net.pool.strides;
	int border = layer->net.pool.border;
	assert(CCV_GET_DATA_TYPE(a->type) == CCV_32F);
	int ch = CCV_GET_CHANNEL(a->type);
	int type = CCV_32F | ch;
	ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, rows, cols, type, type, 0);
	int i, j, k, x, y;
	float* ap = a->data.f32;
	float* bp = db->data.f32;
	for (i = 0; i < db->rows; i++)
	{
		const int start_y = ccv_max(i * strides - border, 0) - (i * strides - border);
		const int end_y = size + ccv_min(i * strides + size - border, a->rows) - (i * strides + size - border);
		for (j = 0; j < db->cols; j++)
		{
			const int start_x = ccv_max(j * strides - border, 0) - (j * strides - border);
			const int end_x = size + ccv_min(j * strides + size - border, a->cols) - (j * strides + size - border);
			for (k = 0; k < ch; k++)
			{
				float v = 0;
				for (y = start_y; y < end_y; y++)
					for (x = start_x; x < end_x; x++)
						if (x == start_x && y == start_y)
							v = ap[(j * strides - border + x + (y - border) * a->cols) * ch + k];
						else if (ap[(j * strides - border + x + (y - border) * a->cols) * ch + k] > v)
							v = ap[(j * strides - border + x + (y - border) * a->cols) * ch + k];
				bp[j * ch + k] = v;
			}
		}
		ap += a->cols * ch * strides;
		bp += db->cols * ch;
	}
}

static void _ccv_convnet_average_pool_forward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t** b)
{
	int rows, cols;
	_ccv_convnet_layer_deduce_output_format(layer, &rows, &cols);
	int size = layer->net.pool.size;
	int strides = layer->net.pool.strides;
	int border = layer->net.pool.border;
	assert(CCV_GET_DATA_TYPE(a->type) == CCV_32F);
	int ch = CCV_GET_CHANNEL(a->type);
	int type = CCV_32F | ch;
	ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, rows, cols, type, type, 0);
	int i, j, k, x, y;
	float* ap = a->data.f32;
	float* bp = db->data.f32;
	for (i = 0; i < db->rows; i++)
	{
		const int start_y = ccv_max(i * strides - border, 0) - (i * strides - border);
		const int end_y = size + ccv_min(i * strides + size - border, a->rows) - (i * strides + size - border);
		for (j = 0; j < db->cols; j++)
		{
			const int start_x = ccv_max(j * strides - border, 0) - (j * strides - border);
			const int end_x = size + ccv_min(j * strides + size - border, a->cols) - (j * strides + size - border);
			for (k = 0; k < ch; k++)
			{
				float v = 0;
				for (y = start_y; y < end_y; y++)
					for (x = start_x; x < end_x; x++)
						v += ap[(j * strides - border + x + (y - border) * a->cols) * ch + k];
				bp[j * ch + k] = v / ((end_x - start_x) * (end_y - start_y));
			}
		}
		ap += a->cols * ch * strides;
		bp += db->cols * ch;
	}
}

#ifndef CASE_TESTS

void ccv_convnet_encode(ccv_convnet_t* convnet, ccv_dense_matrix_t** a, ccv_dense_matrix_t** b, int batch)
{
#ifdef HAVE_CUDA
	if (convnet->use_cwc_accel)
		cwc_convnet_encode(convnet, a, b, batch);
	else {
#endif
	assert(batch == 1);
	assert(CCV_GET_CHANNEL((*a)->type) == convnet->channels);
	assert((*a)->rows == convnet->rows);
	assert((*a)->cols == convnet->cols);
	int i;
	// save the last layer of neuron cache in case that we encode to a different matrix
	ccv_dense_matrix_t* out_neuron = convnet->acts[convnet->count - 1];
	convnet->acts[convnet->count - 1] = *b;
	switch(convnet->layers->type)
	{
		case CCV_CONVNET_CONVOLUTIONAL:
			_ccv_convnet_convolutional_forward_propagate(convnet->layers, *a, convnet->count > 1 ? convnet->dropouts[0] : 0, convnet->acts);
			break;
		case CCV_CONVNET_FULL_CONNECT:
			_ccv_convnet_full_connect_forward_propagate(convnet->layers, *a, convnet->count > 1 ? convnet->dropouts[0] : 0, convnet->acts);
			break;
		case CCV_CONVNET_LOCAL_RESPONSE_NORM:
			_ccv_convnet_rnorm_forward_propagate(convnet->layers, *a, convnet->acts, convnet->denoms);
			break;
		case CCV_CONVNET_MAX_POOL:
			_ccv_convnet_max_pool_forward_propagate(convnet->layers, *a, convnet->acts);
			break;
		case CCV_CONVNET_AVERAGE_POOL:
			_ccv_convnet_average_pool_forward_propagate(convnet->layers, *a, convnet->acts);
			break;
	}
	for (i = 1; i < convnet->count; i++)
	{
		ccv_convnet_layer_t* layer = convnet->layers + i;
		ccv_dense_matrix_t* d = i < convnet->count - 1 ? convnet->dropouts[i] : 0;
		switch(layer->type)
		{
			case CCV_CONVNET_CONVOLUTIONAL:
				_ccv_convnet_convolutional_forward_propagate(layer, convnet->acts[i - 1], d, convnet->acts + i);
				break;
			case CCV_CONVNET_FULL_CONNECT:
				_ccv_convnet_full_connect_forward_propagate(layer, convnet->acts[i - 1], d, convnet->acts + i);
				break;
			case CCV_CONVNET_LOCAL_RESPONSE_NORM:
				_ccv_convnet_rnorm_forward_propagate(layer, convnet->acts[i - 1], convnet->acts + i, convnet->denoms + i);
				break;
			case CCV_CONVNET_MAX_POOL:
				_ccv_convnet_max_pool_forward_propagate(layer, convnet->acts[i - 1], convnet->acts + i);
				break;
			case CCV_CONVNET_AVERAGE_POOL:
				_ccv_convnet_average_pool_forward_propagate(layer, convnet->acts[i - 1], convnet->acts + i);
				break;
		}
	}
	if (convnet->acts + convnet->count - 1 != b)
	{
		*b = convnet->acts[convnet->count - 1];
		// restore the last layer of neuron cache
		convnet->acts[convnet->count - 1] = out_neuron;
	}
#ifdef HAVE_CUDA
	}
#endif
}

void ccv_convnet_classify(ccv_convnet_t* convnet, ccv_dense_matrix_t** a, int* labels, int batch)
{
#ifdef HAVE_CUDA
	if (convnet->use_cwc_accel)
		cwc_convnet_classify(convnet, a, labels, batch);
	else {
#endif
	assert(batch == 1);
	ccv_convnet_encode(convnet, a, convnet->acts + convnet->count - 1, 1);
	int i, c = 0;
	ccv_dense_matrix_t* b = convnet->acts[convnet->count - 1];
	int maxc = b->data.f32[0];
	for (i = 1; i < b->rows; i++)
		if (b->data.f32[i] > maxc)
			maxc = b->data.f32[i], c = i;
	labels[0] = c;
#ifdef HAVE_CUDA
	}
#endif
}

#endif

static void _ccv_convnet_compute_softmax(ccv_dense_matrix_t* a, ccv_dense_matrix_t** b, int type)
{
	int ch = CCV_GET_CHANNEL(a->type);
	assert(CCV_GET_DATA_TYPE(a->type) == CCV_32F);
	ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, a->rows, a->cols, CCV_32F | ch, CCV_32F | ch, 0);
	int i;
	float* aptr = a->data.f32;
	float* bptr = db->data.f32;
	double max = aptr[0];
	for (i = 1; i < a->rows * a->cols * ch; i++)
		if (aptr[i] > max)
			max = aptr[i];
	double tt = 0;
	for (i = 0; i < a->rows * a->cols * ch; i++)
		tt += (bptr[i] = expf(aptr[i] - max));
	tt = 1.0 / tt;
	for (i = 0; i < a->rows * a->cols * ch; i++)
		bptr[i] *= tt;
}

// compute back propagated gradient & weight update delta
static void _ccv_convnet_convolutional_backward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t* n, ccv_dense_matrix_t* d, ccv_dense_matrix_t* m, ccv_dense_matrix_t** b, ccv_convnet_layer_t* update_params)
{
	// a is the input gradient (for back prop), d is the dropout,
	// x is the input (for forward prop), b is the output gradient (gradient, or known as propagated error)
	// note that y (the output from forward prop) is not included because the full connect net is simple enough that we don't need it
	int rows, cols;
	_ccv_convnet_layer_deduce_output_format(layer, &rows, &cols);
	int ch = layer->net.convolutional.channels;
	int count = layer->net.convolutional.count;
	int strides = layer->net.convolutional.strides;
	int border = layer->net.convolutional.border;
	int kernel_rows = layer->net.convolutional.rows;
	int kernel_cols = layer->net.convolutional.cols;
	assert(a->rows == rows);
	assert(a->cols == cols);
	assert(CCV_GET_CHANNEL(a->type) == count);
	int a_rows = a->rows, a_cols = a->cols, a_ch = CCV_GET_CHANNEL(a->type);
	a->rows = rows, a->cols = cols, a->type = (a->type - a_ch) | count;
	assert(CCV_GET_CHANNEL(m->type) == ch);
	assert(CCV_GET_DATA_TYPE(m->type) == CCV_32F);
	int i, j, x, y, k;
	// update weight gradient
#define for_block_w(act_block_setup, act_block_begin, act_block_end) \
	for (k = 0; k < count; k++) \
	{ \
		float* mp = m->data.f32; \
		float* ap = a->data.f32 + k; \
		float* np = n->data.f32 + k; \
		float* update_w = update_params->w + k * kernel_rows * kernel_cols * ch; \
		float bias = 0; \
		act_block_setup; \
		for (i = 0; i < rows; i++) \
		{ \
			int comy = ccv_max(i * strides - border, 0) - (i * strides - border); \
			int maxy = kernel_rows - comy - (i * strides + kernel_rows - ccv_min(m->rows + border, i * strides + kernel_rows)); \
			comy *= ch * kernel_cols; \
			for (j = 0; j < cols; j++) \
			{ \
				act_block_begin; \
				if (np[j * count] > 0) \
				{ /* when np is bigger than 0, relu continues to update the weight, otherwise it stops */ \
					float v = ap[j * count]; \
					bias += v; \
					int comx = (ccv_max(j * strides - border, 0) - (j * strides - border)) * ch; \
					int maxx = kernel_cols * ch - comx - (j * strides + kernel_cols - ccv_min(m->cols + border, j * strides + kernel_cols)) * ch; \
					float* w = update_w + comx + comy; \
					float* mpz = mp + ccv_max(j * strides - border, 0) * ch; \
					/* when we have border, we simply do zero padding */ \
					for (y = 0; y < maxy; y++) \
					{ \
						for (x = 0; x < maxx; x++) \
							w[x] += v * mpz[x]; \
						w += kernel_cols * ch; \
						mpz += m->cols * ch; \
					} \
				} \
				act_block_end; \
			} \
			ap += a->cols * count; \
			np += n->cols * count; \
			mp += m->cols * ch * (ccv_max((i + 1) * strides - border, 0) - ccv_max(i * strides - border, 0)); \
		} \
		update_params->bias[k] = bias; \
	}
	ccv_dense_matrix_t* db = 0;
	if (b)
	{
		db = *b = ccv_dense_matrix_renew(*b, m->rows, m->cols, CCV_32F | CCV_GET_CHANNEL(m->type), CCV_32F | CCV_GET_CHANNEL(m->type), 0);
		// clear it up before propagate result
		ccv_zero(db);
	}
#define for_block_b(act_block_setup, act_block_begin, act_block_end) \
	for (k = 0; k < count; k++) \
	{ \
		float* bp = db->data.f32; \
		float* ap = a->data.f32 + k; \
		float* np = n->data.f32 + k; \
		float* layer_w = layer->w + k * kernel_rows * kernel_cols * ch; \
		act_block_setup; \
		for (i = 0; i < rows; i++) \
		{ \
			int comy = ccv_max(i * strides - border, 0) - (i * strides - border); \
			int maxy = kernel_rows - comy - (i * strides + kernel_rows - ccv_min(db->rows + border, i * strides + kernel_rows)); \
			comy *= ch * kernel_cols; \
			for (j = 0; j < cols; j++) \
			{ \
				act_block_begin; \
				if (np[j * count] > 0) \
				{ /* when np is bigger than 0, relu continues to update the weight, otherwise it stops */ \
					float v = ap[j * count]; \
					int comx = (ccv_max(j * strides - border, 0) - (j * strides - border)) * ch; \
					int maxx = kernel_cols * ch - comx - (j * strides + kernel_cols - ccv_min(db->cols + border, j * strides + kernel_cols)) * ch; \
					float* w = layer_w + comx + comy; \
					float* bpz = bp + ccv_max(j * strides - border, 0) * ch; \
					/* when we have border, we simply do zero padding */ \
					for (y = 0; y < maxy; y++) \
					{ \
						for (x = 0; x < maxx; x++) \
							bpz[x] += v * w[x]; \
						w += kernel_cols * ch; \
						bpz += db->cols * ch; \
					} \
				} \
				act_block_end; \
			} \
			ap += a->cols * count; \
			np += n->cols * count; \
			bp += db->cols * ch * (ccv_max((i + 1) * strides - border, 0) - ccv_max(i * strides - border, 0)); \
		} \
	}
	if (d)
	{
#define act_block_setup \
		int* dp = d->data.i32 + k;
#define act_block_begin \
		if (!*dp) \
		{
#define act_block_end \
		} \
		dp += count;
		for_block_w(act_block_setup, act_block_begin, act_block_end);
		if (db)
			for_block_b(act_block_setup, act_block_begin, act_block_end);
#undef act_block_setup
#undef act_block_begin
#undef act_block_end
	} else {
		for_block_w(/* empty act block setup */, /* empty act block begin */, /* empty act block end */);
		if (db)
			for_block_b(/* empty act block setup */, /* empty act block begin */, /* empty act block end */);
	}
#undef for_block_w
#undef for_block_b
	a->rows = a_rows, a->cols = a_cols, a->type = (a->type - CCV_GET_CHANNEL(a->type)) | a_ch;
}

static void _ccv_convnet_full_connect_backward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t* d, ccv_dense_matrix_t* x, ccv_dense_matrix_t** b, ccv_convnet_layer_t* update_params)
{
	// a is the input gradient (for back prop), d is the dropout,
	// x is the input (for forward prop), b is the output gradient (gradient, or known as propagated error)
	// note that y (the output from forward prop) is not included because the full connect net is simple enough that we don't need it
	ccv_dense_matrix_t* db = 0;
	if (b)
		db = *b = ccv_dense_matrix_renew(*b, x->rows, x->cols, CCV_32F | CCV_GET_CHANNEL(x->type), CCV_32F | CCV_GET_CHANNEL(x->type), 0);
	int x_rows = x->rows, x_cols = x->cols, x_ch = CCV_GET_CHANNEL(x->type);
	x->rows = x_rows * x_cols * x_ch, x->cols = 1, x->type = (x->type - x_ch) | CCV_C1;
	x->step = x->cols * CCV_GET_DATA_TYPE_SIZE(x->type);
	ccv_dense_matrix_t w = ccv_dense_matrix(a->rows, x->rows, CCV_32F | CCV_C1, update_params->w, 0);
	ccv_dense_matrix_t* dw = &w;
	if (d)
	{
		int* dptr = d->data.i32;
		float* aptr = a->data.f32;
		float* bptr = update_params->bias;
		int i, j;
		// bias gradient
		for (i = 0; i < a->rows; i++)
			if (dptr[i])
				bptr[i] += aptr[i];
		// weight gradient
		float* dwptr = update_params->w;
		for (i = 0; i < a->rows; i++)
		{
			if (dptr[i])
			{
				float* xptr = x->data.f32;
				for (j = 0; j < x->rows; j++)
					dwptr[j] += aptr[i] * xptr[j];
			}
			dwptr += x->rows;
		}
		// propagate error
		if (db)
		{
			ccv_zero(db);
			float* wptr = layer->w;
			for (i = 0; i < a->rows; i++)
			{
				if (dptr[i])
				{
					float* bptr = db->data.f32;
					for (j = 0; j < db->rows; j++)
						bptr[j] += wptr[j] * aptr[i];
				}
				wptr += x->rows;
			}
		}
	} else {
		// compute bias gradient
		ccv_dense_matrix_t bias = ccv_dense_matrix(a->rows, 1, CCV_32F | CCV_C1, update_params->bias, 0);
		ccv_dense_matrix_t* dbias = &bias;
		ccv_add(a, dbias, (ccv_matrix_t**)&dbias, 0);
		// compute weight gradient
		ccv_gemm(a, x, 1, dw, 1, CCV_B_TRANSPOSE, (ccv_matrix_t**)&dw, 0);
		w = ccv_dense_matrix(a->rows, x->rows, CCV_32F | CCV_C1, layer->w, 0);
		// propagate error
		if (db)
		{
			db->rows = x->rows, db->cols = x->cols, db->type = (db->type - x_ch) | CCV_C1;
			db->step = db->cols * CCV_GET_DATA_TYPE_SIZE(db->type);
			ccv_gemm(&w, a, 1, 0, 0, CCV_A_TRANSPOSE, (ccv_matrix_t**)&db, 0);
			db->rows = x_rows, db->cols = x_cols, db->type = (db->type - CCV_GET_CHANNEL(db->type)) | x_ch;
			db->step = db->cols * CCV_GET_DATA_TYPE_SIZE(db->type) * CCV_GET_CHANNEL(db->type);
		}
	}
	x->rows = x_rows, x->cols = x_cols, x->type = (x->type - CCV_GET_CHANNEL(x->type)) | x_ch;
	x->step = x->cols * CCV_GET_DATA_TYPE_SIZE(x->type) * CCV_GET_CHANNEL(x->type);
}

static void _ccv_convnet_rnorm_backward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t* n, ccv_dense_matrix_t* m, ccv_dense_matrix_t* denoms, ccv_dense_matrix_t** b)
{
	int rows, cols;
	_ccv_convnet_layer_deduce_output_format(layer, &rows, &cols);
	int size = layer->net.rnorm.size;
	float alpha = layer->net.rnorm.alpha;
	float beta = layer->net.rnorm.beta;
	int way = size / 2;
	assert(CCV_GET_DATA_TYPE(a->type) == CCV_32F);
	int ch = CCV_GET_CHANNEL(a->type);
	int type = CCV_32F | ch;
	ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, rows, cols, type, type, 0);
	int i, j, k, x;
	float* ap = a->data.f32;
	float* np = n->data.f32;
	float* mp = m->data.f32;
	float* dp = denoms->data.f32;
	float* bp = db->data.f32;
	for (i = 0; i < db->rows; i++)
	{
		for (j = 0; j < db->cols; j++)
			for (k = 0; k < ch; k++)
			{
				float nom = 0;
				for (x = ccv_max(k - way, 0); x <= ccv_min(k + way, ch - 1); x++)
					nom += -2 * alpha * beta * ap[j * ch + x] * np[j * ch + x] / dp[j * ch + x];
				bp[j * ch + k] = mp[j * ch + k] * nom + ap[j * ch + k] * powf(dp[j * ch + k], -beta);
			}
		ap += a->cols * ch;
		np += n->cols * ch;
		mp += m->cols * ch;
		dp += denoms->cols * ch;
		bp += db->cols * ch;
	}
}

static void _ccv_convnet_max_pool_backward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t* n, ccv_dense_matrix_t* m, ccv_dense_matrix_t** b)
{
	// a is the input gradient (for back prop), y is the output (from forward prop),
	// x is the input (for forward prop), b is the output gradient (gradient, or known as propagated error)
	// pooling layer doesn't need the dropout
	if (b)
	{
		assert(CCV_GET_CHANNEL(a->type) == CCV_GET_CHANNEL(n->type));
		assert(CCV_GET_CHANNEL(a->type) == CCV_GET_CHANNEL(m->type));
		int ch = CCV_GET_CHANNEL(a->type);
		ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, m->rows, m->cols, CCV_32F | ch, CCV_32F | ch, 0);
		ccv_zero(db);
		int size = layer->net.pool.size;
		int strides = layer->net.pool.strides;
		int border = layer->net.pool.border;
		int i, j, k, x, y;
		float* ap = a->data.f32;
		float* bp = db->data.f32;
		float* np = n->data.f32;
		float* mp = m->data.f32;
		for (i = 0; i < a->rows; i++)
		{
			const int start_y = ccv_max(i * strides - border, 0) - (i * strides - border);
			const int end_y = size + ccv_min(i * strides + size - border, db->rows) - (i * strides + size - border);
			for (j = 0; j < a->cols; j++)
			{
				const int start_x = ccv_max(j * strides - border, 0) - (j * strides - border);
				const int end_x = size + ccv_min(j * strides + size - border, db->cols) - (j * strides + size - border);
				for (k = 0; k < ch; k++)
				{
					float v = np[j * ch + k];
					float u = ap[j * ch + k];
					for (y = start_y; y < end_y; y++)
						for (x = start_x; x < end_x; x++)
							// we have to do direct comparison otherwise it will contribute to too many cells
							// and the propagation won't work. But CPU will have different result comparing with GPU
							if (mp[(j * strides - border + x + (y - border) * m->cols) * ch + k] == v)
								bp[(j * strides - border + x + (y - border) * db->cols) * ch + k] += u;
				}
			}
			ap += a->cols * ch;
			np += n->cols * ch;
			bp += db->cols * ch * strides;
			mp += m->cols * ch * strides;
		}
	}
}

static void _ccv_convnet_average_pool_backward_propagate(ccv_convnet_layer_t* layer, ccv_dense_matrix_t* a, ccv_dense_matrix_t* m, ccv_dense_matrix_t** b)
{
	// a is the input gradient (for back prop), y is the output (from forward prop),
	// x is the input (for forward prop), b is the output gradient (gradient, or known as propagated error)
	// pooling layer doesn't need the dropout
	if (b)
	{
		assert(CCV_GET_CHANNEL(a->type) == CCV_GET_CHANNEL(m->type));
		int ch = CCV_GET_CHANNEL(a->type);
		ccv_dense_matrix_t* db = *b = ccv_dense_matrix_renew(*b, m->rows, m->cols, CCV_32F | ch, CCV_32F | ch, 0);
		ccv_zero(db);
		int size = layer->net.pool.size;
		int strides = layer->net.pool.strides;
		int border = layer->net.pool.border;
		int i, j, k, x, y;
		float* ap = a->data.f32;
		float* bp = db->data.f32;
		for (i = 0; i < a->rows; i++)
		{
			const int start_y = ccv_max(i * strides - border, 0) - (i * strides - border);
			const int end_y = size + ccv_min(i * strides + size - border, db->rows) - (i * strides + size - border);
			for (j = 0; j < a->cols; j++)
			{
				const int start_x = ccv_max(j * strides - border, 0) - (j * strides - border);
				const int end_x = size + ccv_min(j * strides + size - border, db->cols) - (j * strides + size - border);
				for (k = 0; k < ch; k++)
				{
					float u = ap[j * ch + k] / ((end_x - start_x) * (end_y - start_y));
					for (y = start_y; y < end_y; y++)
						for (x = start_x; x < end_x; x++)
							bp[(j * strides - border + x + (y - border) * db->cols) * ch + k] += u;
				}
			}
			ap += a->cols * ch;
			bp += db->cols * ch * strides;
		}
	}
}

static void _ccv_convnet_propagate_loss(ccv_convnet_t* convnet, ccv_dense_matrix_t* a, ccv_dense_matrix_t* dloss, ccv_convnet_t* update_params)
{
	int i;
	ccv_convnet_layer_t* layer = convnet->layers + convnet->count - 1;
	assert(layer->type == CCV_CONVNET_FULL_CONNECT); // the last layer has too be a full connect one to generate softmax result
	_ccv_convnet_full_connect_backward_propagate(layer, dloss, 0, convnet->acts[convnet->count - 2], convnet->count - 1 > 0 ? update_params->acts + convnet->count - 2 : 0, update_params->layers + convnet->count - 1);
	for (i = convnet->count - 2; i >= 0; i--)
	{
		layer = convnet->layers + i;
		switch (layer->type)
		{
			case CCV_CONVNET_CONVOLUTIONAL:
				_ccv_convnet_convolutional_backward_propagate(layer, update_params->acts[i], convnet->acts[i], convnet->dropouts[i], i > 0 ? convnet->acts[i - 1] : a, i > 0 ? update_params->acts + i - 1 : 0, update_params->layers + i);
				break;
			case CCV_CONVNET_FULL_CONNECT:
				_ccv_convnet_full_connect_backward_propagate(layer, update_params->acts[i], convnet->dropouts[i], i > 0 ? convnet->acts[i - 1] : a, i > 0 ? update_params->acts + i - 1 : 0, update_params->layers + i);
				break;
			case CCV_CONVNET_LOCAL_RESPONSE_NORM:
				_ccv_convnet_rnorm_backward_propagate(layer, update_params->acts[i], convnet->acts[i], i > 0 ? convnet->acts[i - 1] : a, convnet->denoms[i], i > 0 ? update_params->acts + i - 1 : 0);
				break;
			case CCV_CONVNET_MAX_POOL:
				_ccv_convnet_max_pool_backward_propagate(layer, update_params->acts[i], convnet->acts[i], i > 0 ? convnet->acts[i - 1] : a, i > 0 ? update_params->acts + i - 1 : 0);
				break;
			case CCV_CONVNET_AVERAGE_POOL:
				_ccv_convnet_average_pool_backward_propagate(layer, update_params->acts[i], i > 0 ? convnet->acts[i - 1] : a, i > 0 ? update_params->acts + i - 1 : 0);
				break;
		}
	}
}

static void _ccv_convnet_update(ccv_convnet_t* convnet, ccv_convnet_t* momentum, ccv_convnet_t* update_params, ccv_convnet_layer_train_param_t* layer_params)
{
	int i, j;
	for (i = 0; i < convnet->count; i++)
		switch (update_params->layers[i].type)
		{
			case CCV_CONVNET_CONVOLUTIONAL:
			{
				float* w = convnet->layers[i].w;
				float* vw = momentum->layers[i].w;
				float* dw = update_params->layers[i].w;
				for (j = 0; j < convnet->layers[i].wnum; j++)
				{
					vw[j] = layer_params[i].w.momentum * vw[j] - layer_params[i].w.decay * layer_params[i].w.learn_rate * w[j] + layer_params[i].w.learn_rate * dw[j];
					w[j] += vw[j];
				}
				float* bias = convnet->layers[i].bias;
				float* vbias = momentum->layers[i].bias;
				float* dbias = update_params->layers[i].bias;
				for (j = 0; j < convnet->layers[i].net.convolutional.count; j++)
				{
					vbias[j] = layer_params[i].bias.momentum * vbias[j] - layer_params[i].bias.decay * layer_params[i].bias.learn_rate * bias[j] + layer_params[i].bias.learn_rate * dbias[j];
					bias[j] += vbias[j];
				}
				break;
			}
			case CCV_CONVNET_FULL_CONNECT:
			{
				float* w = convnet->layers[i].w;
				float* vw = momentum->layers[i].w;
				float* dw = update_params->layers[i].w;
				for (j = 0; j < convnet->layers[i].wnum; j++)
				{
					vw[j] = layer_params[i].w.momentum * vw[j] - layer_params[i].w.decay * layer_params[i].w.learn_rate * w[j] + layer_params[i].w.learn_rate * dw[j];
					w[j] += vw[j];
				}
				float* bias = convnet->layers[i].bias;
				float* vbias = momentum->layers[i].bias;
				float* dbias = update_params->layers[i].bias;
				for (j = 0; j < convnet->layers[i].net.full_connect.count; j++)
				{
					vbias[j] = layer_params[i].bias.momentum * vbias[j] - layer_params[i].bias.decay * layer_params[i].bias.learn_rate * bias[j] + layer_params[i].bias.learn_rate * dbias[j];
					bias[j] += vbias[j];
				}
				break;
			}
		}
}

static void _ccv_convnet_update_zero(ccv_convnet_t* update_params)
{
	int i;
	for (i = 0; i < update_params->count; i++)
		switch (update_params->layers[i].type)
		{
			case CCV_CONVNET_CONVOLUTIONAL:
				memset(update_params->layers[i].w, 0, sizeof(float) * update_params->layers[i].wnum);
				memset(update_params->layers[i].bias, 0, sizeof(float) * update_params->layers[i].net.convolutional.count);
				break;
			case CCV_CONVNET_FULL_CONNECT:
				assert(update_params->layers[i].wnum % update_params->layers[i].net.full_connect.count == 0);
				memset(update_params->layers[i].w, 0, sizeof(float) * update_params->layers[i].wnum);
				memset(update_params->layers[i].bias, 0, sizeof(float) * update_params->layers[i].net.full_connect.count);
				break;
		}
}

static ccv_convnet_t* _ccv_convnet_update_new(ccv_convnet_t* convnet)
{
	ccv_convnet_t* update_params = (ccv_convnet_t*)ccmalloc(sizeof(ccv_convnet_t) + sizeof(ccv_convnet_layer_t) * convnet->count + sizeof(ccv_dense_matrix_t*) * (convnet->count - 1));
	update_params->reserved = 0;
	update_params->layers = (ccv_convnet_layer_t*)(update_params + 1);
	update_params->acts = (ccv_dense_matrix_t**)(update_params->layers + convnet->count);
	// the update params doesn't need the neuron layers (acts) for the input image, and the loss layer, therefore, convnet->count - 1
	memset(update_params->acts, 0, sizeof(ccv_dense_matrix_t*) * (convnet->count - 1));
	update_params->dropouts = 0;
	update_params->rows = convnet->rows;
	update_params->cols = convnet->cols;
	update_params->count = convnet->count;
	update_params->channels = convnet->channels;
	int i;
	for (i = 0; i < convnet->count; i++)
	{
		update_params->layers[i].type = convnet->layers[i].type;
		update_params->layers[i].net = convnet->layers[i].net;
		update_params->layers[i].wnum = convnet->layers[i].wnum;
		switch (update_params->layers[i].type)
		{
			case CCV_CONVNET_CONVOLUTIONAL:
				update_params->layers[i].w = (float*)cccalloc(sizeof(float), update_params->layers[i].wnum + update_params->layers[i].net.convolutional.count);
				update_params->layers[i].bias = update_params->layers[i].w + update_params->layers[i].wnum;
				break;
			case CCV_CONVNET_FULL_CONNECT:
				assert(update_params->layers[i].wnum % update_params->layers[i].net.full_connect.count == 0);
				update_params->layers[i].w = (float*)cccalloc(sizeof(float), update_params->layers[i].wnum + update_params->layers[i].net.full_connect.count);
				update_params->layers[i].bias = update_params->layers[i].w + update_params->layers[i].wnum;
				break;
			case CCV_CONVNET_MAX_POOL:
			case CCV_CONVNET_AVERAGE_POOL:
				update_params->layers[i].w = 0;
				update_params->layers[i].bias = 0;
				break;
		}
	}
	return update_params;
}

#ifndef CASE_TESTS

void ccv_convnet_supervised_train(ccv_convnet_t* convnet, ccv_array_t* categorizeds, ccv_array_t* tests, ccv_convnet_train_param_t params)
{
#ifdef HAVE_CUDA
	if (convnet->use_cwc_accel)
		cwc_convnet_supervised_train(convnet, categorizeds, tests, params);
	else {
#endif
	int i, j, t;
	gsl_rng_env_setup();
	gsl_rng* rng = gsl_rng_alloc(gsl_rng_default);
	int aligned_padding = categorizeds->rnum % params.mini_batch;
	int aligned_rnum = categorizeds->rnum - aligned_padding;
	int* idx = (int*)ccmalloc(sizeof(int) * (categorizeds->rnum + aligned_padding));
	for (i = 0; i < categorizeds->rnum; i++)
		idx[i] = i;
	gsl_ran_shuffle(rng, idx, categorizeds->rnum, sizeof(int));
	// the last layer has to be full connect, thus we can use it as softmax layer
	assert(convnet->layers[convnet->count - 1].type == CCV_CONVNET_FULL_CONNECT);
	int category_count = convnet->layers[convnet->count - 1].net.full_connect.count;
	ccv_convnet_t* update_params = _ccv_convnet_update_new(convnet);
	ccv_convnet_t* momentum = _ccv_convnet_update_new(convnet);
	for (t = 0; t < params.max_epoch; t++)
	{
		for (i = 0; i < aligned_rnum; i++)
		{
			// dropout the first hidden layer
			ccv_categorized_t* categorized = (ccv_categorized_t*)ccv_array_get(categorizeds, idx[i]);
			ccv_convnet_encode(convnet, &categorized->matrix, convnet->acts + convnet->count - 1, 1);
			ccv_dense_matrix_t* softmax = convnet->acts[convnet->count - 1];
			float* dloss = softmax->data.f32;
			_ccv_convnet_compute_softmax(softmax, &softmax, 0);
			assert(softmax->rows == category_count && softmax->cols == 1);
			// this mashes softmax and logistic regression together
			// also, it gives you -D[loss w.r.t. to x_i] (note the negative sign)
			for (j = 0; j < category_count; j++)
				dloss[j] = (j == categorized->c) - dloss[j];
			_ccv_convnet_propagate_loss(convnet, categorized->matrix, softmax, update_params);
			if ((i + 1) % params.mini_batch == 0)
			{
				FLUSH(" - at epoch %03d / %d => stochastic gradient descent at %d / %d", t + 1, params.max_epoch, (i + 1) / params.mini_batch, aligned_rnum / params.mini_batch);
				// update weights
				_ccv_convnet_update(convnet, momentum, update_params, params.layer_params);
				_ccv_convnet_update_zero(update_params);
			}
		}
		int miss = 0;
		for (i = 0; i < tests->rnum; i++)
		{
			FLUSH(" - at epoch %03d / %d => going through %d / %d for tests", t + 1, params.max_epoch, i + 1, tests->rnum);
			ccv_categorized_t* test = (ccv_categorized_t*)ccv_array_get(tests, i);
			int c = 0;
			ccv_convnet_classify(convnet, &test->matrix, &c, 1);
			if (c != test->c)
				++miss;
		}
		FLUSH(" - at epoch %03d / %d => with miss rate %.2f%%\n", t + 1, params.max_epoch, miss * 100.0f / tests->rnum);
		if (t + 1 < params.max_epoch)
		{
			// reshuffle the parts we visited and move the rest to the beginning
			memcpy(idx + categorizeds->rnum, idx + aligned_rnum, sizeof(int) * aligned_padding);
			memmove(idx + aligned_padding, idx, sizeof(int) * aligned_rnum);
			memcpy(idx, idx + categorizeds->rnum, sizeof(int) * aligned_padding);
			gsl_ran_shuffle(rng, idx + aligned_padding, aligned_rnum, sizeof(int));
		}
	}
	ccfree(idx);
	ccv_convnet_free(momentum);
	ccv_convnet_free(update_params);
	gsl_rng_free(rng);
#ifdef HAVE_CUDA
	}
#endif
}

void ccv_convnet_free(ccv_convnet_t* convnet)
{
#ifdef HAVE_CUDA
	cwc_convnet_free(convnet);
#else
	ccfree(convnet);
#endif
}

#endif
