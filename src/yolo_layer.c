#include "yolo_layer.h"
#include "activations.h"
#include "blas.h"
#include "box.h"
#include "dark_cuda.h"
#include "utils.h"

#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

extern int check_mistakes;

layer make_yolo_layer(int batch, int w, int h, int n, int total, int *mask, int classes, int max_boxes)
{
    int i;
    layer l = { (LAYER_TYPE)0 };
    l.type = YOLO;

    l.n = n;
    l.total = total;
    l.batch = batch;
    l.h = h;
    l.w = w;
    l.c = n*(classes + 4 + 1);
    l.out_w = l.w;
    l.out_h = l.h;
    l.out_c = l.c;
    l.classes = classes;
    l.cost = (float*)xcalloc(1, sizeof(float));
    l.biases = (float*)xcalloc(total * 2, sizeof(float));
    if(mask) l.mask = mask;
    else{
        l.mask = (int*)xcalloc(n, sizeof(int));
        for(i = 0; i < n; ++i){
            l.mask[i] = i;
        }
    }
    l.bias_updates = (float*)xcalloc(n * 2, sizeof(float));
    l.outputs = h*w*n*(classes + 4 + 1);
    l.inputs = l.outputs;
    l.max_boxes = max_boxes;
    l.truth_size = 4 + 2;
    l.truths = l.max_boxes*l.truth_size;    // 90*(4 + 1);
    l.labels = (int*)xcalloc(batch * l.w*l.h*l.n, sizeof(int));
    for (i = 0; i < batch * l.w*l.h*l.n; ++i) l.labels[i] = -1;
    l.class_ids = (int*)xcalloc(batch * l.w*l.h*l.n, sizeof(int));
    for (i = 0; i < batch * l.w*l.h*l.n; ++i) l.class_ids[i] = -1;

    l.delta = (float*)xcalloc(batch * l.outputs, sizeof(float));
    l.output = (float*)xcalloc(batch * l.outputs, sizeof(float));
    for(i = 0; i < total*2; ++i){
        l.biases[i] = .5;
    }

    l.forward = forward_yolo_layer;
    l.backward = backward_yolo_layer;
#ifdef GPU
    l.forward_gpu = forward_yolo_layer_gpu;
    l.backward_gpu = backward_yolo_layer_gpu;
    l.output_gpu = cuda_make_array(l.output, batch*l.outputs);
    l.output_avg_gpu = cuda_make_array(l.output, batch*l.outputs);
    l.delta_gpu = cuda_make_array(l.delta, batch*l.outputs);

    free(l.output);
    if (cudaSuccess == cudaHostAlloc(&l.output, batch*l.outputs*sizeof(float), cudaHostRegisterMapped)) l.output_pinned = 1;
    else {
        cudaGetLastError(); // reset CUDA-error
        l.output = (float*)xcalloc(batch * l.outputs, sizeof(float));
    }

    free(l.delta);
    if (cudaSuccess == cudaHostAlloc(&l.delta, batch*l.outputs*sizeof(float), cudaHostRegisterMapped)) l.delta_pinned = 1;
    else {
        cudaGetLastError(); // reset CUDA-error
        l.delta = (float*)xcalloc(batch * l.outputs, sizeof(float));
    }
#endif

    fprintf(stderr, "yolo\n");
    srand(time(0));

    return l;
}

void resize_yolo_layer(layer *l, int w, int h)
{
    l->w = w;
    l->h = h;

    l->outputs = h*w*l->n*(l->classes + 4 + 1);
    l->inputs = l->outputs;

    if (l->embedding_output) l->embedding_output = (float*)xrealloc(l->output, l->batch * l->embedding_size * l->n * l->h * l->w * sizeof(float));
    if (l->labels) l->labels = (int*)xrealloc(l->labels, l->batch * l->n * l->h * l->w * sizeof(int));
    if (l->class_ids) l->class_ids = (int*)xrealloc(l->class_ids, l->batch * l->n * l->h * l->w * sizeof(int));

    if (!l->output_pinned) l->output = (float*)xrealloc(l->output, l->batch*l->outputs * sizeof(float));
    if (!l->delta_pinned) l->delta = (float*)xrealloc(l->delta, l->batch*l->outputs*sizeof(float));

#ifdef GPU
    if (l->output_pinned) {
        CHECK_CUDA(cudaFreeHost(l->output));
        if (cudaSuccess != cudaHostAlloc(&l->output, l->batch*l->outputs * sizeof(float), cudaHostRegisterMapped)) {
            cudaGetLastError(); // reset CUDA-error
            l->output = (float*)xcalloc(l->batch * l->outputs, sizeof(float));
            l->output_pinned = 0;
        }
    }

    if (l->delta_pinned) {
        CHECK_CUDA(cudaFreeHost(l->delta));
        if (cudaSuccess != cudaHostAlloc(&l->delta, l->batch*l->outputs * sizeof(float), cudaHostRegisterMapped)) {
            cudaGetLastError(); // reset CUDA-error
            l->delta = (float*)xcalloc(l->batch * l->outputs, sizeof(float));
            l->delta_pinned = 0;
        }
    }

    cuda_free(l->delta_gpu);
    cuda_free(l->output_gpu);
    cuda_free(l->output_avg_gpu);

    l->delta_gpu =     cuda_make_array(l->delta, l->batch*l->outputs);
    l->output_gpu =    cuda_make_array(l->output, l->batch*l->outputs);
    l->output_avg_gpu = cuda_make_array(l->output, l->batch*l->outputs);
#endif
}

box get_yolo_box(float *x, float *biases, int n, int index, int i, int j, int lw, int lh, int w, int h, int stride, int new_coords)
{
    box b;
    // ln - natural logarithm (base = e)
    // x` = t.x * lw - i;   // x = ln(x`/(1-x`))   // x - output of previous conv-layer
    // y` = t.y * lh - i;   // y = ln(y`/(1-y`))   // y - output of previous conv-layer
    // w = ln(t.w * net.w / anchors_w); // w - output of previous conv-layer
    // h = ln(t.h * net.h / anchors_h); // h - output of previous conv-layer
    if (new_coords) {
        b.x = (i + x[index + 0 * stride]) / lw;
        b.y = (j + x[index + 1 * stride]) / lh;
        b.w = x[index + 2 * stride] * x[index + 2 * stride] * 4 * biases[2 * n] / w;
        b.h = x[index + 3 * stride] * x[index + 3 * stride] * 4 * biases[2 * n + 1] / h;
    }
    else {
        b.x = (i + x[index + 0 * stride]) / lw;
        b.y = (j + x[index + 1 * stride]) / lh;
        b.w = exp(x[index + 2 * stride]) * biases[2 * n] / w;
        b.h = exp(x[index + 3 * stride]) * biases[2 * n + 1] / h;
    }
    return b;
}

static inline float fix_nan_inf(float val)
{
    if (isnan(val) || isinf(val)) val = 0;
    return val;
}

static inline float clip_value(float val, const float max_val)
{
    if (val > max_val) {
        //printf("\n val = %f > max_val = %f \n", val, max_val);
        val = max_val;
    }
    else if (val < -max_val) {
        //printf("\n val = %f < -max_val = %f \n", val, -max_val);
        val = -max_val;
    }
    return val;
}

ious delta_yolo_box(box truth, float *x, float *biases, int n, int index, int i, int j, int lw, int lh, int w, int h, float *delta, float scale, int stride, float iou_normalizer, IOU_LOSS iou_loss, int accumulate, float max_delta, int *rewritten_bbox, int new_coords)
{
    if (delta[index + 0 * stride] || delta[index + 1 * stride] || delta[index + 2 * stride] || delta[index + 3 * stride]) {
        (*rewritten_bbox)++;
    }

    ious all_ious = { 0 };
    // i - step in layer width
    // j - step in layer height
    //  Returns a box in absolute coordinates
    box pred = get_yolo_box(x, biases, n, index, i, j, lw, lh, w, h, stride, new_coords);
    all_ious.iou = box_iou(pred, truth);
    all_ious.giou = box_giou(pred, truth);
    all_ious.diou = box_diou(pred, truth);
    all_ious.ciou = box_ciou(pred, truth);
    // avoid nan in dx_box_iou
    if (pred.w == 0) { pred.w = 1.0; }
    if (pred.h == 0) { pred.h = 1.0; }
    if (iou_loss == MSE)    // old loss
    {
        float tx = (truth.x*lw - i);
        float ty = (truth.y*lh - j);
        float tw = log(truth.w*w / biases[2 * n]);
        float th = log(truth.h*h / biases[2 * n + 1]);

        if (new_coords) {
            //tx = (truth.x*lw - i + 0.5) / 2;
            //ty = (truth.y*lh - j + 0.5) / 2;
            tw = sqrt(truth.w*w / (4 * biases[2 * n]));
            th = sqrt(truth.h*h / (4 * biases[2 * n + 1]));
        }

        //printf(" tx = %f, ty = %f, tw = %f, th = %f \n", tx, ty, tw, th);
        //printf(" x = %f, y = %f, w = %f, h = %f \n", x[index + 0 * stride], x[index + 1 * stride], x[index + 2 * stride], x[index + 3 * stride]);

        // accumulate delta
        delta[index + 0 * stride] += scale * (tx - x[index + 0 * stride]) * iou_normalizer;
        delta[index + 1 * stride] += scale * (ty - x[index + 1 * stride]) * iou_normalizer;
        delta[index + 2 * stride] += scale * (tw - x[index + 2 * stride]) * iou_normalizer;
        delta[index + 3 * stride] += scale * (th - x[index + 3 * stride]) * iou_normalizer;
    }
    else {
        // https://github.com/generalized-iou/g-darknet
        // https://arxiv.org/abs/1902.09630v2
        // https://giou.stanford.edu/
        all_ious.dx_iou = dx_box_iou(pred, truth, iou_loss);

        // jacobian^t (transpose)
        //float dx = (all_ious.dx_iou.dl + all_ious.dx_iou.dr);
        //float dy = (all_ious.dx_iou.dt + all_ious.dx_iou.db);
        //float dw = ((-0.5 * all_ious.dx_iou.dl) + (0.5 * all_ious.dx_iou.dr));
        //float dh = ((-0.5 * all_ious.dx_iou.dt) + (0.5 * all_ious.dx_iou.db));

        // jacobian^t (transpose)
        float dx = all_ious.dx_iou.dt;
        float dy = all_ious.dx_iou.db;
        float dw = all_ious.dx_iou.dl;
        float dh = all_ious.dx_iou.dr;


        // predict exponential, apply gradient of e^delta_t ONLY for w,h
        if (new_coords) {
            //dw *= 8 * x[index + 2 * stride];
            //dh *= 8 * x[index + 3 * stride];
            //dw *= 8 * x[index + 2 * stride] * biases[2 * n] / w;
            //dh *= 8 * x[index + 3 * stride] * biases[2 * n + 1] / h;

            //float grad_w = 8 * exp(-x[index + 2 * stride]) / pow(exp(-x[index + 2 * stride]) + 1, 3);
            //float grad_h = 8 * exp(-x[index + 3 * stride]) / pow(exp(-x[index + 3 * stride]) + 1, 3);
            //dw *= grad_w;
            //dh *= grad_h;
        }
        else {
            dw *= exp(x[index + 2 * stride]);
            dh *= exp(x[index + 3 * stride]);
        }


        //dw *= exp(x[index + 2 * stride]);
        //dh *= exp(x[index + 3 * stride]);

        // normalize iou weight
        dx *= iou_normalizer;
        dy *= iou_normalizer;
        dw *= iou_normalizer;
        dh *= iou_normalizer;


        dx = fix_nan_inf(dx);
        dy = fix_nan_inf(dy);
        dw = fix_nan_inf(dw);
        dh = fix_nan_inf(dh);

        if (max_delta != FLT_MAX) {
            dx = clip_value(dx, max_delta);
            dy = clip_value(dy, max_delta);
            dw = clip_value(dw, max_delta);
            dh = clip_value(dh, max_delta);
        }


        if (!accumulate) {
            delta[index + 0 * stride] = 0;
            delta[index + 1 * stride] = 0;
            delta[index + 2 * stride] = 0;
            delta[index + 3 * stride] = 0;
        }

        // accumulate delta
        delta[index + 0 * stride] += dx;
        delta[index + 1 * stride] += dy;
        delta[index + 2 * stride] += dw;
        delta[index + 3 * stride] += dh;
    }

    return all_ious;
}

void averages_yolo_deltas(int class_index, int box_index, int stride, int classes, float *delta)
{

    int classes_in_one_box = 0;
    int c;
    for (c = 0; c < classes; ++c) {
        if (delta[class_index + stride*c] > 0) classes_in_one_box++;
    }

    if (classes_in_one_box > 0) {
        delta[box_index + 0 * stride] /= classes_in_one_box;
        delta[box_index + 1 * stride] /= classes_in_one_box;
        delta[box_index + 2 * stride] /= classes_in_one_box;
        delta[box_index + 3 * stride] /= classes_in_one_box;
    }
}

void delta_yolo_class(float *output, float *delta, int index, int class_id, int classes, int stride, float *avg_cat, int focal_loss, float label_smooth_eps, float *classes_multipliers, float cls_normalizer)
{
    int n;
    if (delta[index + stride*class_id]){
        float y_true = 1;
        if(label_smooth_eps) y_true = y_true *  (1 - label_smooth_eps) + 0.5*label_smooth_eps;
        float result_delta = y_true - output[index + stride*class_id];
        if(!isnan(result_delta) && !isinf(result_delta)) delta[index + stride*class_id] = result_delta;
        //delta[index + stride*class_id] = 1 - output[index + stride*class_id];

        if (classes_multipliers) delta[index + stride*class_id] *= classes_multipliers[class_id];
        if(avg_cat) *avg_cat += output[index + stride*class_id];
        return;
    }
    // Focal loss
    if (focal_loss) {
        // Focal Loss
        float alpha = 0.5;    // 0.25 or 0.5
        //float gamma = 2;    // hardcoded in many places of the grad-formula

        int ti = index + stride*class_id;
        float pt = output[ti] + 0.000000000000001F;
        // http://fooplot.com/#W3sidHlwZSI6MCwiZXEiOiItKDEteCkqKDIqeCpsb2coeCkreC0xKSIsImNvbG9yIjoiIzAwMDAwMCJ9LHsidHlwZSI6MTAwMH1d
        float grad = -(1 - pt) * (2 * pt*logf(pt) + pt - 1);    // http://blog.csdn.net/linmingan/article/details/77885832
        //float grad = (1 - pt) * (2 * pt*logf(pt) + pt - 1);    // https://github.com/unsky/focal-loss

        for (n = 0; n < classes; ++n) {
            delta[index + stride*n] = (((n == class_id) ? 1 : 0) - output[index + stride*n]);

            delta[index + stride*n] *= alpha*grad;

            if (n == class_id && avg_cat) *avg_cat += output[index + stride*n];
        }
    }
    else {
        // default
        for (n = 0; n < classes; ++n) {
            float y_true = ((n == class_id) ? 1 : 0);
            if (label_smooth_eps) y_true = y_true *  (1 - label_smooth_eps) + 0.5*label_smooth_eps;
            float result_delta = y_true - output[index + stride*n];
            if (!isnan(result_delta) && !isinf(result_delta)) delta[index + stride*n] = result_delta;

            if (classes_multipliers && n == class_id) delta[index + stride*class_id] *= classes_multipliers[class_id] * cls_normalizer;
            if (n == class_id && avg_cat) *avg_cat += output[index + stride*n];
        }
    }
}

int compare_yolo_class(float *output, int classes, int class_index, int stride, float objectness, int class_id, float conf_thresh)
{
    int j;
    for (j = 0; j < classes; ++j) {
        //float prob = objectness * output[class_index + stride*j];
        float prob = output[class_index + stride*j];
        if (prob > conf_thresh) {
            return 1;
        }
    }
    return 0;
}

static int entry_index(layer l, int batch, int location, int entry)
{
    int n =   location / (l.w*l.h);
    int loc = location % (l.w*l.h);
    return batch*l.outputs + n*l.w*l.h*(4+l.classes+1) + entry*l.w*l.h + loc;
}

typedef struct train_yolo_args {
    layer l;
    network_state state;
    int b;

    float tot_iou;
    float tot_giou_loss;
    float tot_iou_loss;
    int count;
    int class_count;
} train_yolo_args;

#include <algorithm>
void *process_batch(void* ptr) {
	train_yolo_args *args = (train_yolo_args*)ptr;
	const layer l = args->l;
	network_state state = args->state;
	int b = args->b;
	int i, j, t, n;
	float avg_cat = 0;

	for (j = 0; j < l.h; ++j) {
		for (i = 0; i < l.w; ++i) {
			for (n = 0; n < l.n; ++n) {
				int offset = n * l.w * l.h + j * l.w + i;
				const int box_index = entry_index(l, b, offset, 0);
				const int obj_index = entry_index(l, b, offset, 4);
				const int class_index = entry_index(l, b, offset, 4 + 1);
				box pred = get_yolo_box(l.output, l.biases, l.mask[n],
						box_index, i, j, l.w, l.h, state.net.w, state.net.h,
						l.w * l.h, l.new_coords);
				float best_match_iou = 0;
				int class_id_match = 0;
				for (int c = 0; c < l.classes; ++c) {
					if (l.output[class_index + l.w * l.h * c] > 0.25) {
						class_id_match = 1;
					}
				}
				for (t = 0; t < l.max_boxes; ++t) {
					box truth = float_to_box_stride(
							state.truth + t * l.truth_size + b * l.truths, 1);
					if (!truth.x) {
						break;
					}
					float objectness = l.output[obj_index];
					if (isnan(objectness) || isinf(objectness)) {
						l.output[obj_index] = 0;
					}
					float iou = box_iou(pred, truth);
					if (iou > best_match_iou && class_id_match == 1) {
						best_match_iou = iou;
					}
				}
				if (best_match_iou <= l.ignore_thresh) {
					l.delta[obj_index] = l.obj_normalizer * (0 - l.output[obj_index]);
				}
			}
		}
	}
	for (t = 0; t < l.max_boxes; ++t) {
		float *cur_truth = state.truth + b * l.truths + t * l.truth_size;
		box truth = float_to_box_stride(cur_truth, 1);
		if (!truth.x) {
			break;
		}
		int class_id = cur_truth[4];
		if (class_id >= l.classes || class_id < 0) {
			continue;
		}

		float best_iou = 0;
		int best_n = 0;
		i = truth.x * l.w;
		j = truth.y * l.h;
		box truth_shift = truth;
		truth_shift.x = truth_shift.y = 0;
		for (n = 0; n < l.total; ++n) {
			box pred = { 0 };
			pred.w = l.biases[2 * n] / state.net.w;
			pred.h = l.biases[2 * n + 1] / state.net.h;
			float iou = box_iou(pred, truth_shift);
			if (iou > best_iou) {
				best_iou = iou;
				best_n = n;
			}
		}

		int mask_n = int_index(l.mask, best_n, l.n);
		if (mask_n >= 0) {
 			int class_id = cur_truth[4];
			if (l.map) {
				class_id = l.map[class_id];
			}
			int offset = mask_n * l.w * l.h + j * l.w + i;
			int box_index = entry_index(l, b, offset, 0);
			delta_yolo_box(truth, l.output, l.biases, best_n,
					box_index, i, j, l.w, l.h, state.net.w, state.net.h,
					l.delta, (2 - truth.w * truth.h), l.w * l.h,
					l.iou_normalizer, l.iou_loss, 1, l.max_delta,
					state.net.rewritten_bbox, l.new_coords);
			++*state.net.total_bbox;

			const int truth_out_index = b * l.n * l.w * l.h + offset;
			l.labels[truth_out_index] = cur_truth[5]; // track_id: hash
			l.class_ids[truth_out_index] = class_id;

			int obj_index = entry_index(l, b, offset, 4);
			l.delta[obj_index] = 1 - l.output[obj_index];

			int class_index = entry_index(l, b, offset, 5);
			delta_yolo_class(l.output, l.delta, class_index, class_id,
					l.classes, l.w * l.h, &avg_cat, l.focal_loss,
					l.label_smooth_eps, l.classes_multipliers, l.cls_normalizer);

			++(args->count);
			++(args->class_count);
		}
	}
	return 0;
}

void forward_yolo_layer(const layer l, network_state state) {
	memcpy(l.output, state.input, l.outputs * l.batch * sizeof(float));
#ifndef GPU
	for (int b = 0; b < l.batch; ++b) {
		for (int n = 0; n < l.n; ++n) {
			int bbox_index = entry_index(l, b, n*l.w*l.h, 0);
			activate_array(l.output + bbox_index, 2 * l.w*l.h, LOGISTIC);        // x,y,
			int obj_index = entry_index(l, b, n*l.w*l.h, 4);
			activate_array(l.output + obj_index, (1 + l.classes)*l.w*l.h, LOGISTIC);
			scal_add_cpu(2 * l.w*l.h, l.scale_x_y, -0.5*(l.scale_x_y - 1), l.output + bbox_index, 1);
		}
	}
#endif
	memset(l.delta, 0, l.outputs * l.batch * sizeof(float));
	if (!state.train) {
		return;
	}
	for (int i = 0; i < l.batch * l.w * l.h * l.n; ++i) {
		l.labels[i] = -1;
		l.class_ids[i] = -1;
	}
	for (int b = 0; b < l.batch; b++) {
		train_yolo_args yolo_args;
		memset(&yolo_args, 0, sizeof(yolo_args));
		yolo_args.l = l;
		yolo_args.state = state;
		yolo_args.b = b;
		process_batch(&yolo_args);
	}
	
	*l.cost = pow(mag_array(l.delta, l.outputs * l.batch), 2);
	printf("iter=%d, chw=[%d,%d,%d], loss=%f\n", *state.net.cur_iteration,
			l.c, l.h, l.w, *l.cost / l.batch);
}

void backward_yolo_layer(const layer l, network_state state)
{
   axpy_cpu(l.batch*l.inputs, 1, l.delta, 1, state.delta, 1);
}

// Converts output of the network to detection boxes
// w,h: image width,height
// netw,neth: network width,height
// relative: 1 (all callers seems to pass TRUE)
void correct_yolo_boxes(detection *dets, int n, int w, int h, int netw, int neth, int relative, int letter)
{
    int i;
    // network height (or width)
    int new_w = 0;
    // network height (or width)
    int new_h = 0;
    // Compute scale given image w,h vs network w,h
    // I think this "rotates" the image to match network to input image w/h ratio
    // new_h and new_w are really just network width and height
    if (letter) {
        if (((float)netw / w) < ((float)neth / h)) {
            new_w = netw;
            new_h = (h * netw) / w;
        }
        else {
            new_h = neth;
            new_w = (w * neth) / h;
        }
    }
    else {
        new_w = netw;
        new_h = neth;
    }
    // difference between network width and "rotated" width
    float deltaw = netw - new_w;
    // difference between network height and "rotated" height
    float deltah = neth - new_h;
    // ratio between rotated network width and network width
    float ratiow = (float)new_w / netw;
    // ratio between rotated network width and network width
    float ratioh = (float)new_h / neth;
    for (i = 0; i < n; ++i) {

        box b = dets[i].bbox;
        // x = ( x - (deltaw/2)/netw ) / ratiow;
        //   x - [(1/2 the difference of the network width and rotated width) / (network width)]
        b.x = (b.x - deltaw / 2. / netw) / ratiow;
        b.y = (b.y - deltah / 2. / neth) / ratioh;
        // scale to match rotation of incoming image
        b.w *= 1 / ratiow;
        b.h *= 1 / ratioh;

        // relative seems to always be == 1, I don't think we hit this condition, ever.
        if (!relative) {
            b.x *= w;
            b.w *= w;
            b.y *= h;
            b.h *= h;
        }

        dets[i].bbox = b;
    }
}

/*
void correct_yolo_boxes(detection *dets, int n, int w, int h, int netw, int neth, int relative, int letter)
{
    int i;
    int new_w=0;
    int new_h=0;
    if (letter) {
        if (((float)netw / w) < ((float)neth / h)) {
            new_w = netw;
            new_h = (h * netw) / w;
        }
        else {
            new_h = neth;
            new_w = (w * neth) / h;
        }
    }
    else {
        new_w = netw;
        new_h = neth;
    }
    for (i = 0; i < n; ++i){
        box b = dets[i].bbox;
        b.x =  (b.x - (netw - new_w)/2./netw) / ((float)new_w/netw);
        b.y =  (b.y - (neth - new_h)/2./neth) / ((float)new_h/neth);
        b.w *= (float)netw/new_w;
        b.h *= (float)neth/new_h;
        if(!relative){
            b.x *= w;
            b.w *= w;
            b.y *= h;
            b.h *= h;
        }
        dets[i].bbox = b;
    }
}
*/

int yolo_num_detections(layer l, float thresh)
{
    int i, n;
    int count = 0;
    for(n = 0; n < l.n; ++n){
        for (i = 0; i < l.w*l.h; ++i) {
            int obj_index  = entry_index(l, 0, n*l.w*l.h + i, 4);
            if(l.output[obj_index] > thresh){
                ++count;
            }
        }
    }
    return count;
}

int yolo_num_detections_batch(layer l, float thresh, int batch)
{
    int i, n;
    int count = 0;
    for (i = 0; i < l.w*l.h; ++i){
        for(n = 0; n < l.n; ++n){
            int obj_index  = entry_index(l, batch, n*l.w*l.h + i, 4);
            if(l.output[obj_index] > thresh){
                ++count;
            }
        }
    }
    return count;
}

void avg_flipped_yolo(layer l)
{
    int i,j,n,z;
    float *flip = l.output + l.outputs;
    for (j = 0; j < l.h; ++j) {
        for (i = 0; i < l.w/2; ++i) {
            for (n = 0; n < l.n; ++n) {
                for(z = 0; z < l.classes + 4 + 1; ++z){
                    int i1 = z*l.w*l.h*l.n + n*l.w*l.h + j*l.w + i;
                    int i2 = z*l.w*l.h*l.n + n*l.w*l.h + j*l.w + (l.w - i - 1);
                    float swap = flip[i1];
                    flip[i1] = flip[i2];
                    flip[i2] = swap;
                    if(z == 0){
                        flip[i1] = -flip[i1];
                        flip[i2] = -flip[i2];
                    }
                }
            }
        }
    }
    for(i = 0; i < l.outputs; ++i){
        l.output[i] = (l.output[i] + flip[i])/2.;
    }
}

int get_yolo_detections(layer l, int w, int h, int netw, int neth, float thresh, int *map, int relative, detection *dets, int letter)
{
    //printf("\n l.batch = %d, l.w = %d, l.h = %d, l.n = %d \n", l.batch, l.w, l.h, l.n);
    int i,j,n;
    float *predictions = l.output;
    // This snippet below is not necessary
    // Need to comment it in order to batch processing >= 2 images
    //if (l.batch == 2) avg_flipped_yolo(l);
    int count = 0;
    for (i = 0; i < l.w*l.h; ++i){
        int row = i / l.w;
        int col = i % l.w;
        for(n = 0; n < l.n; ++n){
            int obj_index  = entry_index(l, 0, n*l.w*l.h + i, 4);
            float objectness = predictions[obj_index];
            //if(objectness <= thresh) continue;    // incorrect behavior for Nan values
            if (objectness > thresh) {
                //printf("\n objectness = %f, thresh = %f, i = %d, n = %d \n", objectness, thresh, i, n);
                int box_index = entry_index(l, 0, n*l.w*l.h + i, 0);
                dets[count].bbox = get_yolo_box(predictions, l.biases, l.mask[n], box_index, col, row, l.w, l.h, netw, neth, l.w*l.h, l.new_coords);
                dets[count].objectness = objectness;
                dets[count].classes = l.classes;
                if (l.embedding_output) {
                    get_embedding(l.embedding_output, l.w, l.h, l.n*l.embedding_size, l.embedding_size, col, row, n, 0, dets[count].embeddings);
                }

                for (j = 0; j < l.classes; ++j) {
                    int class_index = entry_index(l, 0, n*l.w*l.h + i, 4 + 1 + j);
                    float prob = objectness*predictions[class_index];
                    dets[count].prob[j] = (prob > thresh) ? prob : 0;
                }
                ++count;
            }
        }
    }
    correct_yolo_boxes(dets, count, w, h, netw, neth, relative, letter);
    return count;
}

int get_yolo_detections_batch(layer l, int w, int h, int netw, int neth, float thresh, int *map, int relative, detection *dets, int letter, int batch)
{
    int i,j,n;
    float *predictions = l.output;
    //if (l.batch == 2) avg_flipped_yolo(l);
    int count = 0;
    for (i = 0; i < l.w*l.h; ++i){
        int row = i / l.w;
        int col = i % l.w;
        for(n = 0; n < l.n; ++n){
            int obj_index  = entry_index(l, batch, n*l.w*l.h + i, 4);
            float objectness = predictions[obj_index];
            //if(objectness <= thresh) continue;    // incorrect behavior for Nan values
            if (objectness > thresh) {
                //printf("\n objectness = %f, thresh = %f, i = %d, n = %d \n", objectness, thresh, i, n);
                int box_index = entry_index(l, batch, n*l.w*l.h + i, 0);
                dets[count].bbox = get_yolo_box(predictions, l.biases, l.mask[n], box_index, col, row, l.w, l.h, netw, neth, l.w*l.h, l.new_coords);
                dets[count].objectness = objectness;
                dets[count].classes = l.classes;
                if (l.embedding_output) {
                    get_embedding(l.embedding_output, l.w, l.h, l.n*l.embedding_size, l.embedding_size, col, row, n, batch, dets[count].embeddings);
                }

                for (j = 0; j < l.classes; ++j) {
                    int class_index = entry_index(l, batch, n*l.w*l.h + i, 4 + 1 + j);
                    float prob = objectness*predictions[class_index];
                    dets[count].prob[j] = (prob > thresh) ? prob : 0;
                }
                ++count;
            }
        }
    }
    correct_yolo_boxes(dets, count, w, h, netw, neth, relative, letter);
    return count;
}

#ifdef GPU

void forward_yolo_layer_gpu(const layer l, network_state state)
{
    if (l.embedding_output) {
        layer le = state.net.layers[l.embedding_layer_id];
        cuda_pull_array_async(le.output_gpu, l.embedding_output, le.batch*le.outputs);
    }

    //copy_ongpu(l.batch*l.inputs, state.input, 1, l.output_gpu, 1);
    simple_copy_ongpu(l.batch*l.inputs, state.input, l.output_gpu);
    int b, n;
    for (b = 0; b < l.batch; ++b){
        for(n = 0; n < l.n; ++n){
            int bbox_index = entry_index(l, b, n*l.w*l.h, 0);
            // y = 1./(1. + exp(-x))
            // x = ln(y/(1-y))  // ln - natural logarithm (base = e)
            // if(y->1) x -> inf
            // if(y->0) x -> -inf
            if (l.new_coords) {
                //activate_array_ongpu(l.output_gpu + bbox_index, 4 * l.w*l.h, LOGISTIC);    // x,y,w,h
            }
            else {
                activate_array_ongpu(l.output_gpu + bbox_index, 2 * l.w*l.h, LOGISTIC);    // x,y

                int obj_index = entry_index(l, b, n*l.w*l.h, 4);
                activate_array_ongpu(l.output_gpu + obj_index, (1 + l.classes)*l.w*l.h, LOGISTIC); // classes and objectness
            }
            if (l.scale_x_y != 1) scal_add_ongpu(2 * l.w*l.h, l.scale_x_y, -0.5*(l.scale_x_y - 1), l.output_gpu + bbox_index, 1);      // scale x,y
        }
    }
    if(!state.train || l.onlyforward){
        //cuda_pull_array(l.output_gpu, l.output, l.batch*l.outputs);
        if (l.mean_alpha && l.output_avg_gpu) mean_array_gpu(l.output_gpu, l.batch*l.outputs, l.mean_alpha, l.output_avg_gpu);
        cuda_pull_array_async(l.output_gpu, l.output, l.batch*l.outputs);
        CHECK_CUDA(cudaPeekAtLastError());
        return;
    }

    float *in_cpu = (float *)xcalloc(l.batch*l.inputs, sizeof(float));
    cuda_pull_array(l.output_gpu, l.output, l.batch*l.outputs);
    memcpy(in_cpu, l.output, l.batch*l.outputs*sizeof(float));
    float *truth_cpu = 0;
    if (state.truth) {
        int num_truth = l.batch*l.truths;
        truth_cpu = (float *)xcalloc(num_truth, sizeof(float));
        cuda_pull_array(state.truth, truth_cpu, num_truth);
    }
    network_state cpu_state = state;
    cpu_state.net = state.net;
    cpu_state.index = state.index;
    cpu_state.train = state.train;
    cpu_state.truth = truth_cpu;
    cpu_state.input = in_cpu;
    forward_yolo_layer(l, cpu_state);
    //forward_yolo_layer(l, state);
    cuda_push_array(l.delta_gpu, l.delta, l.batch*l.outputs);
    free(in_cpu);
    if (cpu_state.truth) free(cpu_state.truth);
}

void backward_yolo_layer_gpu(const layer l, network_state state)
{
    axpy_ongpu(l.batch*l.inputs, state.net.loss_scale * l.delta_normalizer, l.delta_gpu, 1, state.delta, 1);
}
#endif
